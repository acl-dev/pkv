#include "stdafx.h"
#include "mdb_tbb.h"
#ifdef HAS_TBB
#include "db/detached_pages.h"

namespace pkv {
db::batch_status mdb_tbb::dbdetach_pages(const std::string& owner,
    std::unique_ptr<prepared_pages>& out) {
    out.reset();
    try {
        auto found = find(owner);
        // Family plans keep the owner locked until restore or publication.
        out.reset(new detached_pages<page_map>(found ? &found->pages : nullptr));
        return batch_status::ok;
    } catch (const std::bad_alloc&) { return batch_status::error; }
}
} // namespace pkv
#endif
