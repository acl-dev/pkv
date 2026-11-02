#include "stdafx.h"
#include "json_list.h"
#include "common/redis_integer.h"
#include <algorithm>

namespace pkv {
namespace dao {

bool json_list::read_list(shared_db& db, const std::string& key) {
    if (!read(db, key)) return false;
    if (view_->header().kind != value_kind::list) {
        read_status_ = read_status::wrong_type;
        return false;
    }
    return true;
}

bool json_list::mutate(shared_db& db, const std::string& key, const mutator& callback) {
    reset();
    bool valid = true;
    const bool ok = db->update(key, [&](bool raw_found, std::string& bytes) {
        bool found = raw_found;
        if (found && !decode(db, bytes)) {
            if (read_status_ == read_status::missing) found = false;
            else { valid = false; return db::update_action::keep; }
        }
        value_record record;
        record.header.kind = value_kind::list;
        if (found) {
            if (view_->header().kind != value_kind::list) {
                read_status_ = read_status::wrong_type;
                valid = false;
                return db::update_action::keep;
            }
            if (!materialize(record)) { valid = false; return db::update_action::keep; }
        }
        db::update_action action = db::update_action::keep;
        if (!callback(found, record, action)) { valid = false; return db::update_action::keep; }
        if (action == db::update_action::write) {
            if (record.list_value.empty()) action = db::update_action::erase;
            else if (!encode(db, record, bytes)) { valid = false; return db::update_action::keep; }
        }
        if (action == db::update_action::keep && raw_found && !found) return db::update_action::erase;
        return action;
    });
    if (!ok) read_status_ = read_status::storage_error;
    return ok && valid;
}

bool json_list::push(shared_db& db, const std::string& key, const std::vector<std::string>& values,
        bool left, bool only_existing, long long& length) {
    length = 0;
    return mutate(db, key, [&](bool found, value_record& record, db::update_action& action) {
        if (!found && only_existing) return true;
        auto& items = record.list_value;
        if (left) items.insert(items.begin(), values.rbegin(), values.rend());
        else items.insert(items.end(), values.begin(), values.end());
        length = static_cast<long long>(items.size());
        if (!values.empty()) action = db::update_action::write;
        return true;
    });
}

bool json_list::pop(shared_db& db, const std::string& key, bool left, long long count, list_result& out) {
    out = list_result();
    if (count < 0) { reset(); read_status_ = read_status::invalid_integer; return false; }
    return mutate(db, key, [&](bool found, value_record& record, db::update_action& action) {
        out.found = found;
        if (!found || !count) return true;
        auto& items = record.list_value;
        const auto take = static_cast<size_t>(std::min(count, static_cast<long long>(items.size())));
        out.values.reserve(take);
        if (left) {
            for (size_t i = 0; i < take; ++i) out.values.push_back(std::move(items[i]));
            items.erase(items.begin(), items.begin() + take);
        } else {
            for (size_t i = 0; i < take; ++i) out.values.push_back(std::move(items[items.size() - i - 1]));
            items.resize(items.size() - take);
        }
        action = db::update_action::write;
        return true;
    });
}

bool json_list::length(shared_db& db, const std::string& key, long long& out) {
    out = 0;
    if (!read_list(db, key)) return read_status_ == read_status::missing;
    size_t size;
    if (!view_->list_size(size)) { read_status_ = read_status::invalid_record; return false; }
    out = static_cast<long long>(size);
    return true;
}

bool json_list::range(shared_db& db, const std::string& key, long long start, long long end,
        std::vector<std::string>& out) {
    out.clear();
    if (!read_list(db, key)) return read_status_ == read_status::missing;
    if (!view_->list_range(start, end, out)) { read_status_ = read_status::invalid_record; return false; }
    return true;
}

bool json_list::index(shared_db& db, const std::string& key, const std::string& index_text, list_result& out) {
    out = list_result();
    if (!read_list(db, key)) return read_status_ == read_status::missing;
    long long index_value;
    if (!parse_redis_integer(index_text, index_value)) { read_status_ = read_status::invalid_integer; return false; }
    size_t size, offset;
    if (!view_->list_size(size)) { read_status_ = read_status::invalid_record; return false; }
    if (!list_index(size, index_value, offset)) return true;
    const auto position = static_cast<long long>(offset);
    if (!view_->list_range(position, position, out.values) || out.values.size() != 1) {
        read_status_ = read_status::invalid_record;
        return false;
    }
    out.found = true;
    return true;
}

bool json_list::set(shared_db& db, const std::string& key, const std::string& index_text, const std::string& value) {
    return mutate(db, key, [&](bool found, value_record& record, db::update_action& action) {
        if (!found) { read_status_ = read_status::no_key; return false; }
        long long index_value;
        if (!parse_redis_integer(index_text, index_value)) { read_status_ = read_status::invalid_integer; return false; }
        size_t offset;
        if (!list_index(record.list_value.size(), index_value, offset)) { read_status_ = read_status::index_range; return false; }
        record.list_value[offset] = value;
        action = db::update_action::write;
        return true;
    });
}

bool json_list::trim(shared_db& db, const std::string& key, long long start, long long end) {
    return mutate(db, key, [&](bool found, value_record& record, db::update_action& action) {
        if (!found) return true;
        auto& items = record.list_value;
        size_t offset, count;
        if (!list_slice(items.size(), start, end, offset, count)) action = db::update_action::erase;
        else if (offset != 0 || count != items.size()) {
            items.erase(items.begin() + offset + count, items.end());
            items.erase(items.begin(), items.begin() + offset);
            action = db::update_action::write;
        }
        return true;
    });
}

bool json_list::insert(shared_db& db, const std::string& key, bool before, const std::string& pivot,
        const std::string& value, long long& length) {
    length = 0;
    return mutate(db, key, [&](bool found, value_record& record, db::update_action& action) {
        if (!found) return true;
        auto& items = record.list_value;
        auto position = std::find(items.begin(), items.end(), pivot);
        if (position == items.end()) { length = -1; return true; }
        items.insert(before ? position : position + 1, value);
        length = static_cast<long long>(items.size());
        action = db::update_action::write;
        return true;
    });
}

bool json_list::remove(shared_db& db, const std::string& key, long long count, const std::string& value,
        long long& removed) {
    removed = 0;
    if (count == LLONG_MIN) { reset(); read_status_ = read_status::list_count_range; return false; }
    return mutate(db, key, [&](bool found, value_record& record, db::update_action& action) {
        if (!found) return true;
        auto& items = record.list_value;
        const auto limit = count < 0 ? -count : count;
        const auto matches = [&](const std::string& item) {
            if ((limit == 0 || removed < limit) && item == value) { ++removed; return true; }
            return false;
        };
        if (count < 0) items.erase(items.begin(), std::remove_if(items.rbegin(), items.rend(), matches).base());
        else items.erase(std::remove_if(items.begin(), items.end(), matches), items.end());
        if (removed) action = db::update_action::write;
        return true;
    });
}

bool json_list::positions(shared_db& db, const std::string& key, const std::string& value,
        const list_search& options, std::vector<long long>& out) {
    out.clear();
    if (!read_list(db, key)) return read_status_ == read_status::missing;
    if (!view_->list_positions(value, options, out)) { read_status_ = read_status::invalid_record; return false; }
    return true;
}

bool json_list::batch_result(db::batch_status status) {
    if (status == db::batch_status::error) read_status_ = read_status::storage_error;
    else if (status == db::batch_status::unsupported) read_status_ = read_status::unsupported_operation;
    return status == db::batch_status::ok;
}

bool json_list::decode_list(shared_db& db, const std::string& bytes, long long now, value_record& out) {
    if (!decode(db, bytes, now)) return false;
    if (view_->header().kind != value_kind::list) { read_status_ = read_status::wrong_type; return false; }
    if (!materialize(out)) return false;
    if (out.list_value.empty()) { read_status_ = read_status::invalid_record; return false; }
    return true;
}

bool json_list::move(shared_db& db, const std::string& source, const std::string& destination,
        bool from_left, bool to_left, list_result& out) {
    reset();
    out = list_result();
    const auto status = db->transact({source, destination}, [&](db::batch_view entries) {
        const auto now = unix_time_ms();
        auto& src = *entries.find(source);
        value_record source_record;
        if (!src.found) return true;
        if (!decode_list(db, src.value, now, source_record)) return read_status_ == read_status::missing;
        auto& items = source_record.list_value;
        out.found = true;
        out.values.push_back(from_left ? items.front() : items.back());
        const auto& item = out.values.front();
        if (source == destination) {
            if (from_left == to_left || items.size() == 1) return true;
            if (from_left) std::rotate(items.begin(), items.begin() + 1, items.end());
            else std::rotate(items.begin(), items.end() - 1, items.end());
            if (!encode(db, source_record, src.value)) return false;
            src.action = db::update_action::write;
            return true;
        }
        auto& dst = *entries.find(destination);
        value_record destination_record;
        destination_record.header.kind = value_kind::list;
        if (dst.found && !decode_list(db, dst.value, now, destination_record) && read_status_ != read_status::missing) return false;
        // A missing/expired destination starts without an expiry. Existing
        // source and destination deadlines remain attached to their own keys.
        auto& target = destination_record.list_value;
        if (to_left) target.insert(target.begin(), item);
        else target.push_back(item);
        if (from_left) items.erase(items.begin());
        else items.pop_back();
        if (!encode(db, destination_record, dst.value)) return false;
        dst.action = db::update_action::write;
        if (items.empty()) src.action = db::update_action::erase;
        else {
            if (!encode(db, source_record, src.value)) return false;
            src.action = db::update_action::write;
        }
        return true;
    });
    if (!batch_result(status)) { out = list_result(); return false; }
    return true;
}

bool json_list::mpop(shared_db& db, const std::vector<std::string>& keys, bool left, long long count,
        std::string& selected_key, list_result& out) {
    reset();
    out = list_result();
    selected_key.clear();
    if (count <= 0 || keys.empty()) { read_status_ = read_status::invalid_integer; return false; }
    const auto status = db->transact(keys, [&](db::batch_view entries) {
        const auto now = unix_time_ms();
        // Preserve request order despite the transaction's sorted/deduplicated
        // lock view. Later wrong-type values are irrelevant after a match.
        for (const auto& key : keys) {
            auto& entry = *entries.find(key);
            if (!entry.found) continue;
            value_record record;
            if (!decode_list(db, entry.value, now, record)) {
                if (read_status_ == read_status::missing) continue;
                return false;
            }
            auto& items = record.list_value;
            const auto take = static_cast<size_t>(std::min(count, static_cast<long long>(items.size())));
            out.found = true;
            selected_key = key;
            out.values.reserve(take);
            if (left) {
                for (size_t i = 0; i < take; ++i) out.values.push_back(std::move(items[i]));
                items.erase(items.begin(), items.begin() + take);
            } else {
                for (size_t i = 0; i < take; ++i) out.values.push_back(std::move(items[items.size() - i - 1]));
                items.resize(items.size() - take);
            }
            if (items.empty()) entry.action = db::update_action::erase;
            else {
                if (!encode(db, record, entry.value)) return false;
                entry.action = db::update_action::write;
            }
            return true;
        }
        return true;
    });
    if (!batch_result(status)) { out = list_result(); selected_key.clear(); return false; }
    return true;
}

} // namespace dao
} // namespace pkv
