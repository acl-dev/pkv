#include "stdafx.h"
#include "json_set.h"
#include "common/set_sampling.h"
#include <new>
#include <stdexcept>
#include <algorithm>
#include <iterator>

namespace pkv {
namespace dao {

bool json_set::read_set(shared_db& db, const std::string& key) {
    if (!read(db, key)) return false;
    if (view_->header().kind != value_kind::set) { read_status_ = read_status::wrong_type; return false; }
    return true;
}

bool json_set::decode_set(shared_db& db, const std::string& bytes, long long now, value_record& out) {
    if (!decode(db, bytes, now)) return false;
    if (view_->header().kind != value_kind::set) { read_status_ = read_status::wrong_type; return false; }
    if (!materialize(out)) return false;
    if (out.set_value.empty()) { read_status_ = read_status::invalid_record; return false; }
    return true;
}

bool json_set::batch_result(db::batch_status status) {
    if (status == db::batch_status::error) read_status_ = read_status::storage_error;
    else if (status == db::batch_status::unsupported) read_status_ = read_status::unsupported_operation;
    return status == db::batch_status::ok;
}

bool json_set::change(shared_db& db, const std::string& key, const std::vector<std::string>& values,
        bool add, long long& changed) {
    reset();
    changed = 0;
    bool valid = true;
    const bool ok = db->update(key, [&](bool raw_found, std::string& bytes) {
        bool found = raw_found;
        value_record record;
        record.header.kind = value_kind::set;
        if (found && !decode_set(db, bytes, unix_time_ms(), record)) {
            if (read_status_ == read_status::missing) found = false;
            else { valid = false; return db::update_action::keep; }
        }
        if (!found && !add) return raw_found ? db::update_action::erase : db::update_action::keep;
        for (const auto& value : values) changed += add ? record.set_value.insert(value).second : record.set_value.erase(value);
        if (!changed) return raw_found && !found ? db::update_action::erase : db::update_action::keep;
        if (record.set_value.empty()) return db::update_action::erase;
        if (!encode(db, record, bytes)) { valid = false; return db::update_action::keep; }
        return db::update_action::write;
    });
    if (!ok) read_status_ = read_status::storage_error;
    if (!ok || !valid) changed = 0;
    return ok && valid;
}

bool json_set::size(shared_db& db, const std::string& key, long long& count) {
    count = 0;
    if (!read_set(db, key)) return read_status_ == read_status::missing;
    size_t length;
    if (!view_->set_size(length)) { read_status_ = read_status::invalid_record; return false; }
    count = static_cast<long long>(length);
    return true;
}

bool json_set::membership(shared_db& db, const std::string& key, const std::vector<std::string>& values,
        std::vector<long long>& out) {
    out.clear();
    if (!read_set(db, key)) {
        if (read_status_ != read_status::missing) return false;
        out.assign(values.size(), 0);
        return true;
    }
    if (!view_->set_membership(values, out) || out.size() != values.size()) {
        out.clear(); read_status_ = read_status::invalid_record; return false;
    }
    return true;
}

bool json_set::members(shared_db& db, const std::string& key, std::set<std::string>& out) {
    out.clear();
    if (!read_set(db, key)) return read_status_ == read_status::missing;
    if (!view_->set_values(out)) { read_status_ = read_status::invalid_record; return false; }
    return true;
}

bool json_set::move(shared_db& db, const std::string& source, const std::string& destination,
        const std::string& member, bool& moved) {
    reset();
    moved = false;
    const auto status = db->transact({source, destination}, [&](db::batch_view entries) {
        const auto now = unix_time_ms();
        auto& src = *entries.find(source);
        value_record from;
        if (!src.found) return true;
        if (!decode_set(db, src.value, now, from)) return read_status_ == read_status::missing;
        if (source == destination) { moved = from.set_value.count(member) != 0; return true; }
        auto& dst = *entries.find(destination);
        value_record to;
        to.header.kind = value_kind::set;
        if (dst.found && !decode_set(db, dst.value, now, to) && read_status_ != read_status::missing) return false;
        // Redis checks the destination type even if the member is absent.
        if (!from.set_value.erase(member)) return true;
        moved = true;
        if (to.set_value.insert(member).second) {
            if (!encode(db, to, dst.value)) return false;
            dst.action = db::update_action::write;
        }
        if (from.set_value.empty()) src.action = db::update_action::erase;
        else {
            if (!encode(db, from, src.value)) return false;
            src.action = db::update_action::write;
        }
        return true;
    });
    if (!batch_result(status)) { moved = false; return false; }
    return true;
}

bool json_set::algebra(shared_db& db, const std::vector<std::string>& keys, set_operation operation,
        const std::string* destination, std::set<std::string>& out) {
    reset();
    out.clear();
    if (keys.empty()) { read_status_ = read_status::invalid_record; return false; }
    auto locked = keys;
    if (destination) locked.push_back(*destination);
    const auto status = db->transact(std::move(locked), [&](db::batch_view entries) {
        const auto now = unix_time_ms();
        std::map<std::string, std::set<std::string>> snapshots;
        for (const auto& key : keys) {
            if (snapshots.count(key)) continue;
            const auto& entry = *entries.find(key);
            value_record record;
            if (entry.found && !decode_set(db, entry.value, now, record) && read_status_ != read_status::missing) return false;
            snapshots.emplace(key, std::move(record.set_value));
        }
        if (operation == set_operation::unite) {
            for (const auto& snapshot : snapshots) out.insert(snapshot.second.begin(), snapshot.second.end());
        } else if (operation == set_operation::difference) {
            out = snapshots.at(keys.front());
            for (size_t i = 1; i < keys.size(); ++i) {
                for (const auto& member : snapshots.at(keys[i])) out.erase(member);
                if (out.empty()) break;
            }
        } else {
            std::vector<const std::set<std::string>*> ordered;
            for (const auto& snapshot : snapshots) ordered.push_back(&snapshot.second);
            std::sort(ordered.begin(), ordered.end(), [](const std::set<std::string>* a, const std::set<std::string>* b) {
                return a->size() < b->size();
            });
            out = *ordered.front();
            for (size_t i = 1; i < ordered.size() && !out.empty(); ++i) {
                std::set<std::string> next;
                std::set_intersection(out.begin(), out.end(), ordered[i]->begin(), ordered[i]->end(), std::inserter(next, next.end()));
                out.swap(next);
            }
        }
        if (!destination) return true;
        auto& target = *entries.find(*destination);
        if (out.empty()) target.action = db::update_action::erase;
        else {
            value_record record;
            record.header.kind = value_kind::set; // store operations clear the destination TTL
            record.set_value = out;
            if (!encode(db, record, target.value)) return false;
            target.action = db::update_action::write;
        }
        return true;
    });
    if (!batch_result(status)) { out.clear(); return false; }
    return true;
}

bool json_set::choose_sample(const std::set<std::string>& values, long long count, std::vector<std::string>& out) {
    if (!count) { out.clear(); return true; }
    const auto wanted = static_cast<unsigned long long>(count < 0 ? -count : count);
    try {
        if (!sample_set(values, wanted, count < 0, set_sampling_engine(), out)) {
            read_status_ = read_status::sample_too_large;
            return false;
        }
    } catch (const std::bad_alloc&) {
        out.clear(); read_status_ = read_status::storage_error; return false;
    } catch (const std::length_error&) {
        out.clear(); read_status_ = read_status::sample_too_large; return false;
    }
    return true;
}

bool json_set::sample(shared_db& db, const std::string& key, long long count, bool remove, set_sample_result& out) {
    reset();
    out = set_sample_result();
    if (count == LLONG_MIN || (remove && count < 0)) { read_status_ = read_status::invalid_integer; return false; }
    if (!remove) {
        if (!read_set(db, key)) return read_status_ == read_status::missing;
        std::set<std::string> values;
        if (!view_->set_values(values)) { read_status_ = read_status::invalid_record; return false; }
        if (!choose_sample(values, count, out.values)) { out = set_sample_result(); return false; }
        out.found = true;
        return true;
    }
    bool valid = true;
    const bool ok = db->update(key, [&](bool found, std::string& bytes) {
        if (!found) return db::update_action::keep;
        value_record record;
        if (!decode_set(db, bytes, unix_time_ms(), record)) {
            if (read_status_ == read_status::missing) return db::update_action::erase;
            valid = false; return db::update_action::keep;
        }
        out.found = true;
        if (!choose_sample(record.set_value, count, out.values)) { valid = false; return db::update_action::keep; }
        if (out.values.empty()) return db::update_action::keep;
        for (const auto& member : out.values) record.set_value.erase(member);
        if (record.set_value.empty()) return db::update_action::erase;
        if (!encode(db, record, bytes)) { valid = false; return db::update_action::keep; }
        return db::update_action::write;
    });
    if (!ok) read_status_ = read_status::storage_error;
    if (!ok || !valid) out = set_sample_result();
    return ok && valid;
}

bool json_set::intersection_size(shared_db& db, const std::vector<std::string>& keys, long long limit, long long& count) {
    reset();
    count = 0;
    if (keys.empty() || limit < 0) { read_status_ = read_status::invalid_integer; return false; }
    const auto status = db->transact(keys, [&](db::batch_view entries) {
        const auto now = unix_time_ms();
        bool empty = false;
        std::vector<std::unique_ptr<record_view>> owners;
        std::vector<std::pair<size_t, record_view*>> ordered;
        // All source types must be validated before returning for a missing set
        // or reaching LIMIT. Keep owning views rather than copying each set.
        for (const auto& entry : entries) {
            if (!entry.found) { empty = true; continue; }
            if (!decode(db, entry.value, now)) {
                if (read_status_ == read_status::missing) { empty = true; continue; }
                return false;
            }
            if (view_->header().kind != value_kind::set) { read_status_ = read_status::wrong_type; return false; }
            size_t size;
            if (!view_->set_size(size)) { read_status_ = read_status::invalid_record; return false; }
            ordered.emplace_back(size, view_.get());
            owners.push_back(std::move(view_));
        }
        if (empty) return true;
        std::sort(ordered.begin(), ordered.end(), [](const std::pair<size_t, record_view*>& a,
                const std::pair<size_t, record_view*>& b) { return a.first < b.first; });
        bool valid = true;
        const bool scanned = ordered.front().second->set_visit([&](const std::string& candidate) {
            for (size_t i = 1; i < ordered.size(); ++i) {
                bool member;
                if (!ordered[i].second->set_contains(candidate, member)) { valid = false; return false; }
                if (!member) return true;
            }
            ++count;
            return !limit || count < limit;
        });
        if (!scanned || !valid) { read_status_ = read_status::invalid_record; return false; }
        return true;
    });
    if (!batch_result(status)) { count = 0; return false; }
    return true;
}

} // namespace dao
} // namespace pkv
