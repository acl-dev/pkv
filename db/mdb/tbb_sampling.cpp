#include "stdafx.h"
#include "mdb_tbb.h"
#if defined(HAS_TBB)

namespace pkv {
db::scan_status mdb_tbb::dbkey_at_rank(uint64_t rank, std::string& out,
    bool& found) {
    // Exclusive admission protects existing TBB node addresses and scan sets.
    for (const auto& store : stores_) {
        const auto size = store->owners.size();
        if (rank >= size) { rank -= size; continue; }
        for (const auto& slot : store->indexes) {
            const auto* index = slot.load(std::memory_order_acquire);
            if (!index) continue;
            if (rank >= index->keys.size()) {
                rank -= index->keys.size(); continue;
            }
            out = index->keys[static_cast<size_t>(rank)]->first;
            found = true; return scan_status::ok;
        }
        return scan_status::error;
    }
    return scan_status::ok;
}
db::scan_status mdb_tbb::dbvisit_keys(const key_visitor& visit) {
    for (const auto& store : stores_)
        for (const auto& slot : store->indexes) {
            const auto* index = slot.load(std::memory_order_acquire);
            if (!index) continue;
            for (size_t i = 0; i < index->keys.size(); ++i)
                if (!visit(index->keys[i]->first)) return scan_status::ok;
        }
    return scan_status::ok;
}
} // namespace pkv
#endif
