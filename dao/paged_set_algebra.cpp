#include "stdafx.h"
#include "paged_set.h"
#include "coder/compact_set.h"
#include <algorithm>

namespace pkv {
namespace dao {

bool paged_set::accept(stores& sources, const std::string& first,
    const std::string& member, set_operation mode, bool& accepted) {
    accepted = true;
    for (auto& item : sources) {
        if (item.first == first) continue;
        bool found;
        if (!item.second->probe_set_member(member, found)) {
            read_status_ = map_read_status(item.second->error());
            return false;
        }
        if ((mode == set_operation::intersect && !found) ||
            (mode == set_operation::difference && found)) {
            accepted = false; break;
        }
    }
    return true;
}

static bool comparable_grouped_sets(const map_store& left, const map_store& right) {
    return left.metadata().grouped && right.metadata().grouped &&
        right.metadata().elements / 2 <= left.metadata().elements;
}

bool paged_set::compare_grouped_sets(map_store& left, map_store& right,
    set_operation mode, const std::function<void(const std::string&)>& emit) {
    std::vector<std::string> members;
    constexpr size_t bucket_limit = 256;
    constexpr size_t bytes_limit = 128 * 1024;
    for (unsigned bucket = 0; bucket < 256; ++bucket) {
        members.clear();
        bool large_bucket = false;
        size_t bytes = 0;
        const auto collect = [&](const std::string& member) {
            if (large_bucket) return true;
            if (members.size() == bucket_limit || member.size() > bytes_limit - bytes) {
                large_bucket = true;
                members.clear();
            } else {
                if (!members.empty() && members.back() >= member) {
                    read_status_ = read_status::invalid_record;
                    return false;
                }
                bytes += member.size();
                members.push_back(member);
            }
            return true;
        };
        if (!right.visit_set_bucket(bucket, std::cref(collect))) {
            if (read_status_ != read_status::invalid_record) finish(right);
            return false;
        }
        bool probe_failed = false;
        auto next = members.cbegin();
        bool have_previous = false;
        std::string previous;
        const auto compare = [&](const std::string& member) {
            bool found;
            if (large_bucket) {
                if (!right.probe_set_member(member, found)) {
                    probe_failed = true;
                    return false;
                }
            } else {
                if (have_previous && previous >= member) {
                    read_status_ = read_status::invalid_record;
                    return false;
                }
                previous = member;
                have_previous = true;
                while (next != members.cend() && *next < member) ++next;
                found = next != members.cend() && *next == member;
            }
            if (found == (mode == set_operation::intersect)) emit(member);
            return true;
        };
        if (!left.visit_set_bucket(bucket, std::cref(compare))) {
            if (read_status_ != read_status::invalid_record)
                finish(probe_failed ? right : left);
            return false;
        }
    }
    return true;
}

// Returns false before producing output when an input needs the paged path.
static bool compact_algebra(page_transaction& tx,
    const std::vector<std::string>& keys, set_operation operation,
    std::set<std::string>& out) {
    std::vector<compact_set::view> inputs;
    inputs.reserve(keys.size());
    bool missing = false;
    long long now = -1;
    for (size_t i = 0; i < keys.size(); ++i) {
        const auto& root = *tx.roots().find(keys[i]);
        if (root.found) {
            if (!compact_set::matches(root.value)) return false;
            const compact_set::view packed(root.value);
            const auto deadline = packed.deadline();
            if (deadline >= 0 && now < 0) now = unix_time_ms();
            if (deadline < 0 || deadline > now) {
                inputs.push_back(packed);
                continue;
            }
        }
        missing |= operation == set_operation::intersect ||
            (operation == set_operation::difference && i == 0);
    }
    if (missing || inputs.empty()) return true;
    if (operation == set_operation::unite) {
        for (const auto& packed : inputs)
            for (unsigned i = 0; i < packed.count(); ++i)
                out.insert(packed.at(i).str());
        return true;
    }
    const auto candidate = operation == set_operation::difference ? inputs.begin() :
        std::min_element(inputs.begin(), inputs.end(),
            [](const compact_set::view& left, const compact_set::view& right) {
                return left.count() < right.count();
            });
    const bool intersect = operation == set_operation::intersect;
    for (unsigned i = 0; i < candidate->count(); ++i) {
        const auto member = candidate->at(i);
        const auto accepts = [&](const compact_set::view& packed) {
            return &packed == &*candidate || packed.contains(member) == intersect;
        };
        if (std::all_of(inputs.begin(), inputs.end(), accepts))
            out.emplace_hint(out.end(), member.data(), member.size());
    }
    return true;
}

bool paged_set::algebra(shared_db& db,
    const std::vector<std::string>& keys, set_operation mode,
    const std::string* destination, std::set<std::string>& out) {
    reset(); out.clear();
    if (keys.empty()) {
        read_status_ = read_status::invalid_record; return false;
    }
    auto locked = keys;
    if (destination) locked.push_back(*destination);
    const bool ok = result(db->transact_pages(std::move(locked),
        [&](page_transaction& tx) {
        const auto write_target = [&]() {
            if (!destination) return true;
            if (!out.empty() && out.size() <= compact_set::max_members && db->supports_compact_sets()) {
                std::vector<std::string> members(out.begin(), out.end());
                auto bytes = compact_set::encode(members);
                if (!bytes.empty()) {
                    if (!tx.reset_pages(*destination)) {
                        read_status_ = read_status::storage_error;
                        return false;
                    }
                    auto& root = *tx.roots().find(*destination);
                    root.value = std::move(bytes);
                    root.action = db::update_action::write;
                    root.maintenance_only = false;
                    return true;
                }
            }
            map_store target(tx, *destination, map_kind::set);
            // STORE accepts any old destination type and clears its TTL.
            target.initialize();
            if (out.size() >= 128) {
                auto next = out.cbegin();
                const auto member_at = [&](size_t) -> const std::string& {
                    return *next++;
                };
                long long added;
                if (!target.build_set(std::cref(member_at), out.size(), added)) {
                    finish(target);
                    return false;
                }
                return finish(target);
            }
            for (const auto& member : out) {
                bool added;
                if (!target.put(member, "", true, added)) return finish(target);
            }
            return finish(target);
        };
        bool packed_inputs;
        try {
            packed_inputs = compact_algebra(tx, keys, mode, out);
        } catch (const std::runtime_error&) {
            read_status_ = read_status::invalid_record;
            return false;
        }
        if (packed_inputs) return write_target();
        stores values;
        if (!sources(db, tx, keys, values)) return false;
        auto candidate = values.find(keys.front());
        if (mode == set_operation::intersect) {
            candidate = std::min_element(values.begin(), values.end(),
                [](const stores::value_type& a, const stores::value_type& b) {
                return a.second->metadata().elements <
                    b.second->metadata().elements;
            });
        }
        bool probe_failed = false;
        const auto collect = [&](const std::string& member) {
            bool accepted = true;
            if (mode != set_operation::unite && !accept(values,
                candidate->first, member, mode, accepted)) {
                probe_failed = true;
                return false;
            }
            if (accepted) out.insert(member);
            return true;
        };
        const auto scan = [&](map_store& source) {
            if (source.visit_all_set_members(std::cref(collect))) return true;
            if (!probe_failed) finish(source);
            return false;
        };
        // SDIFF with a repeated first input is always empty, but all input
        // types were validated above before this short circuit.
        const bool repeated = mode == set_operation::difference &&
            std::count(keys.begin(), keys.end(), keys.front()) > 1;
        if (mode == set_operation::unite) {
            for (auto& item : values) {
                if (!scan(*item.second)) return false;
            }
        } else if (!repeated) {
            if (values.size() == 2) {
                const auto other = candidate == values.begin() ?
                    std::next(candidate) : values.begin();
                auto& left = *candidate->second;
                auto& right = *other->second;
                if (comparable_grouped_sets(left, right)) {
                    const auto emit = [&](const std::string& member) { out.insert(member); };
                    if (!compare_grouped_sets(left, right, mode, std::cref(emit)))
                        return false;
                } else if (!scan(left)) return false;
            } else if (!scan(*candidate->second)) return false;
        }
        for (auto& item : values) if (!finish(*item.second)) return false;
        return write_target();
    }));
    if (!ok) out.clear();
    return ok;
}

bool paged_set::intersection_size(shared_db& db,
    const std::vector<std::string>& keys, long long limit, long long& count) {
    reset(); count = 0;
    if (keys.empty() || limit < 0) {
        read_status_ = read_status::invalid_integer; return false;
    }
    // A set intersected with itself has the cardinality stored in its root.
    if (std::all_of(keys.begin() + 1, keys.end(),
        [&](const std::string& key) { return key == keys.front(); })) {
        if (!size(db, keys.front(), count)) return false;
        if (limit) count = std::min(limit, count);
        return true;
    }
    const auto evaluate = [&](page_transaction& tx) {
        if (keys.size() == 2 && keys[0] != keys[1]) {
            const auto now = unix_time_ms();
            const auto& first_root = *tx.roots().find(keys[0]);
            const auto& second_root = *tx.roots().find(keys[1]);
            if (first_root.found && second_root.found &&
                compact_set::matches(first_root.value) &&
                compact_set::matches(second_root.value)) {
                try {
                    const compact_set::view left(first_root.value);
                    const compact_set::view right(second_root.value);
                    const auto left_deadline = left.deadline();
                    const auto right_deadline = right.deadline();
                    if ((left_deadline < 0 || left_deadline > now) &&
                        (right_deadline < 0 || right_deadline > now)) {
                        unsigned i = 0, j = 0;
                        while (i < left.count() && j < right.count()) {
                            const auto order = left.at(i).compare(right.at(j));
                            if (order <= 0) ++i;
                            if (order >= 0) ++j;
                            if (order == 0 && ++count == limit) break;
                        }
                        return true;
                    }
                } catch (const std::runtime_error&) {
                    read_status_ = read_status::invalid_record;
                    return false;
                }
            }
            map_store first(tx, keys[0], map_kind::set);
            map_store second(tx, keys[1], map_kind::set);
            if (!open(db, tx, keys[0], first, now) ||
                !open(db, tx, keys[1], second, now)) return false;
            auto* left = &first;
            auto* right = &second;
            const auto a = first.metadata().elements;
            const auto b = second.metadata().elements;
            if (b < a || (a == b && keys[1] < keys[0])) std::swap(left, right);
            if ((!limit || static_cast<uint64_t>(limit) >= left->metadata().elements) &&
                comparable_grouped_sets(*left, *right)) {
                const auto emit = [&](const std::string&) { ++count; };
                return compare_grouped_sets(*left, *right, set_operation::intersect,
                    std::cref(emit)) && finish(*left) && finish(*right);
            }
            bool valid = true;
            const auto probe = [&](const std::string& member, const std::string&) {
                bool found;
                if (!right->probe_set_member(member, found)) {
                    read_status_ = map_read_status(right->error());
                    valid = false;
                    return false;
                }
                if (found) ++count;
                return !limit || count < limit;
            };
            const bool scanned = limit ? left->visit_set_prefix(std::cref(probe)) :
                left->visit(std::cref(probe), false);
            if (!scanned && valid) finish(*left);
            return scanned && valid && finish(*left) && finish(*right);
        }
        stores values;
        if (!sources(db, tx, keys, values)) return false;
        auto candidate = std::min_element(values.begin(), values.end(),
            [](const stores::value_type& a, const stores::value_type& b) {
            return a.second->metadata().elements <
                b.second->metadata().elements;
        });
        auto& left = *candidate->second;
        if (values.size() == 2 && (!limit ||
            static_cast<uint64_t>(limit) >= left.metadata().elements)) {
            const auto other = candidate == values.begin() ?
                std::next(candidate) : values.begin();
            auto& right = *other->second;
            if (comparable_grouped_sets(left, right)) {
                const auto emit = [&](const std::string&) { ++count; };
                return compare_grouped_sets(left, right, set_operation::intersect,
                    std::cref(emit)) && finish(left) && finish(right);
            }
        }
        bool valid = true;
        const auto probe = [&](const std::string& member,
            const std::string&) {
            bool accepted;
            if (!accept(values, candidate->first, member,
                set_operation::intersect, accepted)) {
                valid = false; return false;
            }
            if (accepted) ++count;
            return !limit || count < limit;
        };
        const bool scanned = limit ? left.visit_set_prefix(std::cref(probe)) :
            left.visit(std::cref(probe), false);
        if (!scanned) return finish(left);
        if (!valid) return false;
        for (auto& item : values) if (!finish(*item.second)) return false;
        return true;
    };
    const bool ok = result(db->transact_pages(keys, std::cref(evaluate)));
    if (!ok) count = 0;
    return ok;
}

} // namespace dao
} // namespace pkv
