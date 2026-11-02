#include "stdafx.h"
#include "mdb_tbb.h"
#ifdef HAS_TBB
#include "db/page_transaction.h"

namespace pkv {
bool mdb_tbb::dbsupports_family_transfer(const db& other) const {
    return dynamic_cast<const mdb_tbb*>(&other) != nullptr;
}
db::lookup_status mdb_tbb::dblookup_bounded_root(const std::string& owner,
    std::string& out) {
    const auto found = find(owner);
    if (!found) return lookup_status::missing;
    if (found->root.size() > page_transaction::max_value_bytes)
        return lookup_status::error;
    try { out = found->root; return lookup_status::found; }
    catch (const std::bad_alloc&) { return lookup_status::error; }
}
db::batch_status mdb_tbb::dbvisit_pages(const std::string& owner,
    const page_visitor& visit) {
    const auto found = find(owner);
    if (!found) return batch_status::ok;
    for (const auto& page : found->pages) {
        if (!page.first || page.second.size() >
            page_transaction::max_value_bytes)
            return batch_status::error;
        if (!visit(page.first, page.second)) return batch_status::aborted;
    }
    return batch_status::ok;
}
} // namespace pkv
#endif
