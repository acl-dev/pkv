#include "stdafx.h"
#include "paged_zset.h"
#include <cmath>
#include <cstdlib>
#include <type_traits>

namespace pkv {
namespace dao {

struct paged_zset::algebra_source {
    std::unique_ptr<zset_store> sorted;
    std::unique_ptr<map_store> set;
    std::unique_ptr<record_view> legacy;
    uint64_t size = 0;
    bool visit(const zset_store::visitor& callback) {
        if (sorted) return sorted->visit(callback);
        if (set) return set->visit([&](const std::string& member,
            const std::string&) { return callback(member, 1); }, false);
        if (legacy) return legacy->set_visit([&](const std::string& member) {
            return callback(member, 1);
        });
        return true;
    }
    bool get(const std::string& member, zset_score& out) {
        out = zset_score();
        if (sorted) return sorted->get(member, out);
        out.score = 1;
        if (set) {
            uint64_t bytes;
            return set->length(member, bytes, out.found);
        }
        return !legacy || legacy->set_contains(member, out.found);
    }
};

bool paged_zset::sources(shared_db& db, page_transaction& tx,
    const std::vector<std::string>& keys, source_map& out) {
    const auto now = unix_time_ms();
    for (const auto& key : keys) {
        if (out.count(key)) continue;
        auto source = std::unique_ptr<algebra_source>(new algebra_source);
        auto& entry = *tx.roots().find(key);
        if (entry.found && decode(db, entry.value, now)) {
            const auto kind = view_->header().kind;
            if (kind == value_kind::zset) {
                source->sorted.reset(new zset_store(tx, key));
                if (!open(db, tx, key, *source->sorted, now)) return false;
                source->size = source->sorted->metadata().elements;
            } else if (kind == value_kind::set) {
                if (view_->paged_payload()) {
                    source->set.reset(new map_store(tx, key, map_kind::set));
                    if (!source->set->open()) {
                        read_status_ = map_read_status(source->set->error());
                        return false;
                    }
                    source->size = source->set->metadata().elements;
                } else {
                    size_t count;
                    if (!view_->set_size(count)) {
                        read_status_ = read_status::invalid_record;
                        return false;
                    }
                    source->size = count; source->legacy = std::move(view_);
                }
            } else { read_status_ = read_status::wrong_type; return false; }
        } else if (entry.found && read_status_ != read_status::missing)
            return false;
        out.emplace(key, std::move(source));
    }
    return true;
}

bool paged_zset::source_error(const source_map& sources) {
    read_status_ = read_status::invalid_record;
    for (const auto& item : sources) {
        const auto& source = *item.second;
        const auto error = source.sorted ? source.sorted->error() :
            source.set ? source.set->error() : map_store::status::ok;
        if (error != map_store::status::ok) {
            read_status_ = map_read_status(error); break;
        }
    }
    return false;
}

bool paged_zset::finish(source_map& sources) {
    for (auto& item : sources) {
        auto& source = *item.second;
        if (source.sorted && !finish(*source.sorted)) return false;
        if (source.set && !source.set->finish()) return source_error(sources);
    }
    return true;
}

bool paged_zset::algebra(shared_db& db,
    const std::vector<std::string>& keys, zset_operation operation,
    const std::string* destination, bool cardinality_only,
    const zset_options_parser& configure, std::vector<zset_member>& out,
    long long& cardinality) {
    reset(); out.clear(); cardinality = 0;
    if (keys.empty() || (cardinality_only &&
        (destination || operation != zset_operation::intersect))) {
        read_status_ = read_status::invalid_integer; return false;
    }
    auto locked = keys;
    if (destination) locked.push_back(*destination);
    const bool ok = result(db->transact_pages(std::move(locked),
        [&](page_transaction& tx) {
        source_map snapshots;
        if (!sources(db, tx, keys, snapshots)) return false;
        zset_algebra_options options;
        options.weights.assign(keys.size(), 1);
        if (!configure(options)) return false;
        if (options.weights.size() != keys.size() || options.limit < 0) {
            read_status_ = read_status::invalid_integer; return false;
        }
        for (double weight : options.weights) if (std::isnan(weight)) {
            read_status_ = read_status::invalid_score; return false;
        }
        struct input { algebra_source* source; double weight; };
        static_assert(std::is_trivially_copyable<input>::value,
            "qsort requires trivially copyable input");
        std::vector<input> inputs;
        for (size_t i = 0; i < keys.size(); ++i)
            inputs.push_back({snapshots.at(keys[i]).get(), options.weights[i]});
        if (operation != zset_operation::difference)
            std::qsort(inputs.data(), inputs.size(), sizeof(input),
                [](const void* a, const void* b) {
                const auto first = static_cast<const input*>(a)->source->size;
                const auto second = static_cast<const input*>(b)->source->size;
                return first < second ? -1 : first > second ? 1 : 0;
            });
        const auto aggregate = [&](double current, double value) {
            if (options.aggregate == zset_aggregate::minimum)
                return value < current ? value : current;
            if (options.aggregate == zset_aggregate::maximum)
                return value > current ? value : current;
            const double sum = current + value;
            return std::isnan(sum) ? 0.0 : sum;
        };
        std::map<std::string, double> values;
        bool valid = true;
        if (operation == zset_operation::unite) {
            for (const auto& input : inputs) {
                if (!input.source->visit([&](const std::string& member,
                    double score) {
                    double weighted = score * input.weight;
                    if (std::isnan(weighted)) weighted = 0;
                    auto inserted = values.emplace(member, weighted);
                    if (!inserted.second) inserted.first->second =
                        aggregate(inserted.first->second, weighted);
                    return true;
                })) return source_error(snapshots);
            }
        } else if (inputs.front().source->size) {
            if (!inputs.front().source->visit([&](const std::string& member,
                double score) {
                double total = operation == zset_operation::difference ?
                    score : score * inputs.front().weight;
                if (std::isnan(total)) total = 0;
                for (size_t i = 1; i < inputs.size(); ++i) {
                    zset_score other;
                    if (!inputs[i].source->get(member, other)) {
                        valid = false; return false;
                    }
                    if (operation == zset_operation::difference) {
                        if (other.found) return true;
                    } else {
                        if (!other.found) return true;
                        if (!cardinality_only) total = aggregate(total,
                            other.score * inputs[i].weight);
                    }
                }
                if (cardinality_only) {
                    ++cardinality;
                    return !options.limit || cardinality < options.limit;
                }
                values.emplace(member, total); return true;
            }) || !valid) return source_error(snapshots);
        }
        if (!finish(snapshots)) return false;
        if (cardinality_only) return true;
        cardinality = static_cast<long long>(values.size());
        if (!destination) { out = ordered_zset(values); return true; }
        zset_store target(tx, *destination);
        target.initialize();
        for (const auto& value : values) {
            bool added, changed;
            if (!target.put(value.first, value.second, added, changed))
                return finish(target);
        }
        return finish(target);
    }));
    if (!ok) { out.clear(); cardinality = 0; }
    return ok;
}

} // namespace dao
} // namespace pkv
