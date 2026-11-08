#include "stdafx.h"
#include "mdb.h"

namespace pkv {
db::scan_status mdb::dbkey_at_rank(uint64_t rank, std::string& out,
    bool& found) {
    // Exclusive admission stabilizes borrowed hash-node addresses.
    for (const auto& store : stores_) {
        const auto size = store->keys.size();
        if (rank >= size) { rank -= size; continue; }
        out = store->keys[static_cast<size_t>(rank)]->first;
        found = true; return scan_status::ok;
    }
    return scan_status::ok;
}
db::scan_status mdb::dbvisit_keys(const key_visitor& visit) {
    for (const auto& store : stores_)
        for (size_t i = 0; i < store->keys.size(); ++i)
            if (!visit(store->keys[i]->first)) return scan_status::ok;
    return scan_status::ok;
}
} // namespace pkv
