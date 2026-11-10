#include "stdafx.h"
#include "mdb.h"
#include "db/detached_pages.h"

namespace pkv {
db::batch_status mdb::dbdetach_pages(const std::string& owner,
    std::unique_ptr<prepared_pages>& out) {
    out.reset();
    try {
        const auto position = locate(owner);
        std::lock_guard<lock_t> guard(*locks_[position.shard]);
        auto* family = find_page_family(position.shard, false);
        page_map* pages = nullptr;
        if (family) {
            auto found = family->find(owner);
            if (found != family->end()) pages = &found->second;
        }
        out.reset(new detached_pages<page_map>(pages));
        return batch_status::ok;
    } catch (const std::bad_alloc&) { return batch_status::error; }
}
} // namespace pkv
