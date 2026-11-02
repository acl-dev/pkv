#include "stdafx.h"
#include "paged_set.h"
#include <algorithm>

namespace pkv {
namespace dao {

bool paged_set::accept(stores& sources, const std::string& first,
    const std::string& member, set_operation operation, bool& accepted) {
    accepted = true;
    for (auto& item : sources) {
        if (item.first == first) continue;
        bool found;
        if (!contains(*item.second, member, found)) return false;
        if ((operation == set_operation::intersect && !found) ||
            (operation == set_operation::difference && found)) {
            accepted = false; break;
        }
    }
    return true;
}

bool paged_set::algebra(shared_db& db,
    const std::vector<std::string>& keys, set_operation operation,
    const std::string* destination, std::set<std::string>& out) {
    reset(); out.clear();
    if (keys.empty()) {
        read_status_ = read_status::invalid_record; return false;
    }
    auto locked = keys;
    if (destination) locked.push_back(*destination);
    const bool ok = result(db->transact_pages(std::move(locked),
        [&](page_transaction& tx) {
        stores values;
        if (!sources(db, tx, keys, values)) return false;
        auto candidate = values.find(keys.front());
        if (operation == set_operation::intersect) {
            candidate = std::min_element(values.begin(), values.end(),
                [](const stores::value_type& a, const stores::value_type& b) {
                return a.second->metadata().elements <
                    b.second->metadata().elements;
            });
        }
        bool valid = true;
        const auto collect = [&](const std::string& member,
            const std::string&) {
            bool accepted = true;
            if (operation != set_operation::unite && !accept(values,
                candidate->first, member, operation, accepted)) {
                valid = false; return false;
            }
            if (accepted) out.insert(member);
            return true;
        };
        // SDIFF with a repeated first input is always empty, but all input
        // types were validated above before this short circuit.
        const bool repeated = operation == set_operation::difference &&
            std::count(keys.begin(), keys.end(), keys.front()) > 1;
        if (operation == set_operation::unite) {
            for (auto& item : values) {
                if (!item.second->visit(collect, false))
                    return finish(*item.second);
            }
        } else if (!repeated && !candidate->second->visit(collect, false))
            return finish(*candidate->second);
        if (!valid) return false;
        for (auto& item : values) if (!finish(*item.second)) return false;
        if (!destination) return true;
        map_store target(tx, *destination, map_kind::set);
        // STORE accepts any old destination type and clears its TTL.
        target.initialize();
        for (const auto& member : out) {
            bool added;
            if (!target.put(member, "", true, added)) return finish(target);
        }
        return finish(target);
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
    const bool ok = result(db->transact_pages(keys,
        [&](page_transaction& tx) {
        stores values;
        if (!sources(db, tx, keys, values)) return false;
        auto candidate = std::min_element(values.begin(), values.end(),
            [](const stores::value_type& a, const stores::value_type& b) {
            return a.second->metadata().elements <
                b.second->metadata().elements;
        });
        bool valid = true;
        if (!candidate->second->visit([&](const std::string& member,
            const std::string&) {
            bool accepted;
            if (!accept(values, candidate->first, member,
                set_operation::intersect, accepted)) {
                valid = false; return false;
            }
            if (accepted) ++count;
            return !limit || count < limit;
        }, false)) return finish(*candidate->second);
        if (!valid) return false;
        for (auto& item : values) if (!finish(*item.second)) return false;
        return true;
    }));
    if (!ok) count = 0;
    return ok;
}

} // namespace dao
} // namespace pkv
