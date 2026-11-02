#include "stdafx.h"
#include "json_zset.h"
#include "dao/map_status.h"
#include <cmath>
#include <cstdlib>
#include <type_traits>

namespace pkv {
namespace dao {
namespace {
void extract_zset(value_record& record, long long count, bool maximum, std::vector<zset_member>& out) {
    auto ordered = ordered_zset(record.zset_value);
    if (maximum) std::reverse(ordered.begin(), ordered.end());
    const auto take = static_cast<size_t>(std::min(count, static_cast<long long>(ordered.size())));
    out.assign(ordered.begin(), ordered.begin() + take);
    for (const auto& value : out) record.zset_value.erase(value.member);
}
}

bool json_zset::algebra(shared_db& db, const std::vector<std::string>& keys, zset_operation operation,
        const std::string* destination, bool cardinality_only, const zset_options_parser& configure,
        std::vector<zset_member>& out, long long& cardinality) {
    reset(); out.clear(); cardinality = 0;
    if (keys.empty() || (cardinality_only && (destination || operation != zset_operation::intersect))) {
        read_status_ = read_status::invalid_integer; return false;
    }
    auto locked = keys;
    if (destination) locked.push_back(*destination);
    const auto status = db->transact_page_reads(std::move(locked),
        [&](page_transaction& tx) {
        auto entries = tx.roots();
        struct source {
            std::unique_ptr<record_view> view;
            std::unique_ptr<map_store> native;
            size_t size = 0;
        };
        std::map<std::string, source> snapshots;
        const auto now = unix_time_ms();
        for (const auto& key : keys) {
            if (snapshots.count(key)) continue;
            auto& snapshot = snapshots[key];
            const auto& entry = *entries.find(key);
            if (!entry.found) continue;
            if (!decode(db, entry.value, now)) {
                if (read_status_ == read_status::missing) continue;
                return false;
            }
            const auto kind = view_->header().kind;
            if (kind != value_kind::set && kind != value_kind::zset) { read_status_ = read_status::wrong_type; return false; }
            if (kind == value_kind::set && view_->paged_payload()) {
                snapshot.native.reset(new map_store(tx, key, map_kind::set));
                if (!snapshot.native->open()) {
                    read_status_ = map_read_status(snapshot.native->error());
                    return false;
                }
                snapshot.size = snapshot.native->metadata().elements;
            } else if (!(kind == value_kind::set ?
                view_->set_size(snapshot.size) :
                view_->zset_size(snapshot.size))) {
                read_status_ = read_status::invalid_record; return false;
            }
            snapshot.view = std::move(view_);
        }
        const auto source_error = [&] {
            read_status_ = read_status::invalid_record;
            for (const auto& item : snapshots) {
                if (item.second.native && item.second.native->error() !=
                    map_store::status::ok)
                    read_status_ = map_read_status(
                        item.second.native->error());
            }
            return false;
        };
        zset_algebra_options options;
        options.weights.assign(keys.size(), 1);
        if (!configure(options)) return false;
        if (options.weights.size() != keys.size() || options.limit < 0) { read_status_ = read_status::invalid_integer; return false; }
        for (double weight : options.weights) if (std::isnan(weight)) { read_status_ = read_status::invalid_score; return false; }
        struct input { source* snapshot; double weight; };
        std::vector<input> sources;
        for (size_t i = 0; i < keys.size(); ++i) sources.push_back({&snapshots.at(keys[i]), options.weights[i]});
        static_assert(std::is_trivially_copyable<input>::value, "qsort requires trivially copyable source references");
        if (operation != zset_operation::difference) {
            // Redis uses cardinality-only qsort ordering. Equal-size sources
            // must follow the same platform sort, since SUM is not associative.
            std::qsort(sources.data(), sources.size(), sizeof(input), [](const void* a, const void* b) {
                const auto first = static_cast<const input*>(a)->snapshot->size;
                const auto second = static_cast<const input*>(b)->snapshot->size;
                return first < second ? -1 : first > second ? 1 : 0;
            });
        }
        const auto visit = [&](source& snapshot, const std::function<bool(const std::string&, double)>& callback) {
            if (snapshot.native) return snapshot.native->visit(
                [&](const std::string& member, const std::string&) {
                    return callback(member, 1);
                }, false);
            if (!snapshot.view) return true;
            return snapshot.view->header().kind == value_kind::set ?
                snapshot.view->set_visit([&](const std::string& member) { return callback(member, 1); }) : snapshot.view->zset_visit(callback);
        };
        const auto find = [&](source& snapshot, const std::string& member, zset_score& result) {
            result = zset_score();
            if (snapshot.native) {
                uint64_t bytes;
                result.score = 1;
                return snapshot.native->length(member, bytes, result.found);
            }
            if (!snapshot.view) return true;
            if (snapshot.view->header().kind == value_kind::set) {
                result.score = 1;
                return snapshot.view->set_contains(member, result.found);
            }
            return snapshot.view->zset_lookup(member, result);
        };
        const auto aggregate = [&](double current, double value) {
            if (options.aggregate == zset_aggregate::minimum) return value < current ? value : current;
            if (options.aggregate == zset_aggregate::maximum) return value > current ? value : current;
            const double sum = current + value;
            return std::isnan(sum) ? 0.0 : sum;
        };
        std::map<std::string, double> result;
        bool valid = true;
        if (operation == zset_operation::unite) {
            for (const auto& input : sources) {
                if (!visit(*input.snapshot, [&](const std::string& member, double score) {
                    double weighted = score * input.weight;
                    if (std::isnan(weighted)) weighted = 0;
                    auto inserted = result.emplace(member, weighted);
                    if (!inserted.second) inserted.first->second = aggregate(inserted.first->second, weighted);
                    return true;
                })) return source_error();
            }
        } else if (sources.front().snapshot->size) {
            if (!visit(*sources.front().snapshot, [&](const std::string& member, double score) {
                double total = operation == zset_operation::difference ? score : score * sources.front().weight;
                if (std::isnan(total)) total = 0;
                for (size_t i = 1; i < sources.size(); ++i) {
                    zset_score other;
                    if (!find(*sources[i].snapshot, member, other)) { valid = false; return false; }
                    if (operation == zset_operation::difference) { if (other.found) return true; }
                    else {
                        if (!other.found) return true;
                        if (!cardinality_only) total = aggregate(total, other.score * sources[i].weight);
                    }
                }
                if (cardinality_only) {
                    ++cardinality;
                    return !options.limit || cardinality < options.limit;
                }
                result.emplace(member, total);
                return true;
            }) || !valid) return source_error();
        }
        if (cardinality_only) return true;
        cardinality = static_cast<long long>(result.size());
        if (!destination) { out = ordered_zset(result); return true; }
        auto& target = *entries.find(*destination);
        if (result.empty()) target.action = db::update_action::erase;
        else {
            value_record record;
            record.header.kind = value_kind::zset;
            record.zset_value = std::move(result);
            if (!encode(db, record, target.value)) return false;
            target.action = db::update_action::write;
        }
        return true;
    });
    if (status == db::batch_status::error) read_status_ = read_status::storage_error;
    else if (status == db::batch_status::unsupported) read_status_ = read_status::unsupported_operation;
    if (status != db::batch_status::ok) { out.clear(); cardinality = 0; return false; }
    return true;
}

bool json_zset::read_zset(shared_db& db, const std::string& key) {
    if (!read(db, key)) return false;
    if (view_->header().kind != value_kind::zset) { read_status_ = read_status::wrong_type; return false; }
    return true;
}
bool json_zset::mutate(shared_db& db, const std::string& key, const mutator& callback) {
    reset();
    bool valid = true;
    const bool ok = db->update(key, [&](bool raw_found, std::string& bytes) {
        bool found = raw_found;
        value_record record;
        record.header.kind = value_kind::zset;
        if (found && !decode(db, bytes)) {
            if (read_status_ == read_status::missing) found = false;
            else { valid = false; return db::update_action::keep; }
        }
        if (found) {
            if (view_->header().kind != value_kind::zset) {
                read_status_ = read_status::wrong_type; valid = false; return db::update_action::keep;
            }
            if (!materialize(record)) { valid = false; return db::update_action::keep; }
        }
        db::update_action action = db::update_action::keep;
        if (!callback(found, record, action)) { valid = false; return db::update_action::keep; }
        if (action == db::update_action::write) {
            if (record.zset_value.empty()) action = db::update_action::erase;
            else if (!encode(db, record, bytes)) { valid = false; return db::update_action::keep; }
        }
        if (action == db::update_action::keep && raw_found && !found) return db::update_action::erase;
        return action;
    });
    if (!ok) read_status_ = read_status::storage_error;
    return ok && valid;
}
bool json_zset::add(shared_db& db, const std::string& key, const std::vector<zset_member>& values,
        const zadd_options& options, zadd_result& out) {
    out = zadd_result();
    for (const auto& value : values) if (std::isnan(value.score)) { reset(); read_status_ = read_status::invalid_score; return false; }
    const bool ok = mutate(db, key, [&](bool, value_record& record, db::update_action& action) {
        for (const auto& value : values) {
            auto current = record.zset_value.find(value.member);
            if (current == record.zset_value.end()) {
                if (options.xx) continue;
                record.zset_value.emplace(value.member, value.score);
                ++out.added; ++out.changed; out.processed = true; out.score = value.score;
            } else {
                if (options.nx) continue;
                const auto score = options.incr ? current->second + value.score : value.score;
                if (std::isnan(score)) { read_status_ = read_status::nan_score; return false; }
                if ((options.gt && score <= current->second) || (options.lt && score >= current->second)) continue;
                out.processed = true; out.score = score;
                if (score != current->second) { current->second = score; ++out.changed; }
            }
        }
        if (out.changed) action = db::update_action::write;
        return true;
    });
    if (!ok) out = zadd_result();
    return ok;
}
bool json_zset::remove(shared_db& db, const std::string& key, const std::vector<std::string>& values, long long& removed) {
    removed = 0;
    const bool ok = mutate(db, key, [&](bool found, value_record& record, db::update_action& action) {
        if (!found) return true;
        for (const auto& value : values) removed += record.zset_value.erase(value);
        if (removed) action = db::update_action::write;
        return true;
    });
    if (!ok) removed = 0;
    return ok;
}
bool json_zset::sample(shared_db& db, const std::string& key, long long count, zset_sample_result& out) {
    out = zset_sample_result();
    if (count == LLONG_MIN) { reset(); read_status_ = read_status::invalid_integer; return false; }
    if (!read_zset(db, key)) return read_status_ == read_status::missing;
    if (!count) { out.found = true; return true; }
    size_t size;
    if (!view_->zset_size(size)) { read_status_ = read_status::invalid_record; return false; }
    const auto wanted = static_cast<unsigned long long>(count < 0 ? -count : count);
    const auto length = size == 0 ? 0 : count < 0 ? wanted : std::min(wanted, static_cast<unsigned long long>(size));
    if (length > out.values.max_size()) { read_status_ = read_status::sample_too_large; return false; }
    try {
        if (!view_->zset_sample(length, count < 0, out.values) || out.values.size() != length) {
            out = zset_sample_result(); read_status_ = read_status::invalid_record; return false;
        }
    } catch (const std::bad_alloc&) {
        out = zset_sample_result(); read_status_ = read_status::storage_error; return false;
    } catch (const std::length_error&) {
        out = zset_sample_result(); read_status_ = read_status::sample_too_large; return false;
    }
    out.found = true;
    return true;
}
bool json_zset::size(shared_db& db, const std::string& key, long long& count) {
    count = 0;
    if (!read_zset(db, key)) return read_status_ == read_status::missing;
    size_t length;
    if (!view_->zset_size(length)) { read_status_ = read_status::invalid_record; return false; }
    count = static_cast<long long>(length); return true;
}
bool json_zset::scores(shared_db& db, const std::string& key, const std::vector<std::string>& values, std::vector<zset_score>& out) {
    out.clear();
    if (!read_zset(db, key)) {
        if (read_status_ != read_status::missing) return false;
        out.resize(values.size()); return true;
    }
    if (!view_->zset_scores(values, out) || out.size() != values.size()) { out.clear(); read_status_ = read_status::invalid_record; return false; }
    return true;
}
bool json_zset::range(shared_db& db, const std::string& key, long long start, long long end, bool reverse, std::vector<zset_member>& out) {
    out.clear();
    if (!read_zset(db, key)) return read_status_ == read_status::missing;
    if (!view_->zset_range(start, end, reverse, out)) { read_status_ = read_status::invalid_record; return false; }
    return true;
}
bool json_zset::select(shared_db& db, const std::string& key, const zset_query& query, std::vector<zset_member>& out) {
    out.clear();
    if (!read_zset(db, key)) return read_status_ == read_status::missing;
    if (!view_->zset_select(query, out)) { out.clear(); read_status_ = read_status::invalid_record; return false; }
    return true;
}
bool json_zset::count(shared_db& db, const std::string& key, const zset_query& query, long long& out) {
    out = 0;
    if (!read_zset(db, key)) return read_status_ == read_status::missing;
    if (!view_->zset_count(query, out)) { out = 0; read_status_ = read_status::invalid_record; return false; }
    return true;
}
bool json_zset::remove_range(shared_db& db, const std::string& key, const zset_query& query, long long& removed) {
    removed = 0;
    const bool ok = mutate(db, key, [&](bool found, value_record& record, db::update_action& action) {
        if (!found) return true;
        std::vector<zset_member> selected;
        select_zset(record.zset_value, query, selected);
        for (const auto& value : selected) record.zset_value.erase(value.member);
        removed = static_cast<long long>(selected.size());
        if (removed) action = db::update_action::write;
        return true;
    });
    if (!ok) removed = 0;
    return ok;
}
bool json_zset::store_range(shared_db& db, const std::string& source, const std::string& destination,
        const zset_query& query, long long& stored) {
    reset(); stored = 0;
    const auto status = db->transact({source, destination}, [&](db::batch_view entries) {
        auto& src = *entries.find(source);
        std::vector<zset_member> values;
        if (src.found) {
            if (!decode(db, src.value, unix_time_ms())) {
                if (read_status_ != read_status::missing) return false;
            } else {
                if (view_->header().kind != value_kind::zset) { read_status_ = read_status::wrong_type; return false; }
                if (!view_->zset_select(query, values)) { read_status_ = read_status::invalid_record; return false; }
            }
        }
        auto& dst = *entries.find(destination);
        stored = static_cast<long long>(values.size());
        if (values.empty()) dst.action = db::update_action::erase;
        else {
            value_record result;
            result.header.kind = value_kind::zset;
            for (const auto& value : values) result.zset_value.emplace(value.member, value.score);
            if (!encode(db, result, dst.value)) return false;
            dst.action = db::update_action::write;
        }
        return true;
    });
    if (status == db::batch_status::error) read_status_ = read_status::storage_error;
    else if (status == db::batch_status::unsupported) read_status_ = read_status::unsupported_operation;
    if (status != db::batch_status::ok) { stored = 0; return false; }
    return true;
}

bool json_zset::rank(shared_db& db, const std::string& key, const std::string& member, bool reverse, zset_rank& out) {
    out = zset_rank();
    if (!read_zset(db, key)) return read_status_ == read_status::missing;
    if (!view_->zset_position(member, reverse, out)) { read_status_ = read_status::invalid_record; return false; }
    return true;
}
bool json_zset::mpop(shared_db& db, const std::vector<std::string>& keys, bool maximum, long long count,
        std::string& selected_key, zset_pop_result& out) {
    reset(); selected_key.clear(); out = zset_pop_result();
    if (keys.empty() || count <= 0) { read_status_ = read_status::invalid_integer; return false; }
    const auto status = db->transact(keys, [&](db::batch_view entries) {
        const auto now = unix_time_ms();
        // Lock order is independent of request order; only the first live key
        // is decoded/mutated. Later wrong types must not prevent that pop.
        for (const auto& key : keys) {
            auto& entry = *entries.find(key);
            if (!entry.found) continue;
            if (!decode(db, entry.value, now)) {
                if (read_status_ == read_status::missing) continue;
                return false;
            }
            if (view_->header().kind != value_kind::zset) { read_status_ = read_status::wrong_type; return false; }
            value_record record;
            if (!materialize(record)) return false;
            if (record.zset_value.empty()) { read_status_ = read_status::invalid_record; return false; }
            extract_zset(record, count, maximum, out.values);
            selected_key = key; out.found = true;
            if (record.zset_value.empty()) entry.action = db::update_action::erase;
            else {
                if (!encode(db, record, entry.value)) return false;
                entry.action = db::update_action::write;
            }
            return true;
        }
        return true;
    });
    if (status == db::batch_status::error) read_status_ = read_status::storage_error;
    else if (status == db::batch_status::unsupported) read_status_ = read_status::unsupported_operation;
    if (status != db::batch_status::ok) { out = zset_pop_result(); selected_key.clear(); return false; }
    return true;
}
bool json_zset::pop(shared_db& db, const std::string& key, long long count, bool maximum, std::vector<zset_member>& out) {
    out.clear();
    if (count < 0) { reset(); read_status_ = read_status::invalid_integer; return false; }
    const bool ok = mutate(db, key, [&](bool found, value_record& record, db::update_action& action) {
        if (!found || !count) return true;
        extract_zset(record, count, maximum, out);
        action = db::update_action::write;
        return true;
    });
    if (!ok) out.clear();
    return ok;
}
}
}
