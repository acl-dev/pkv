#include "stdafx.h"
#ifdef HAS_WT
#include "wdb.h"
#include "wt.h"

namespace pkv {
db::lookup_status wdb::dblookup_bounded_root(const std::string& owner,
    std::string& out) {
    return dbs_.empty() ? lookup_status::error :
        dbs_[locate(owner)]->bounded_root(owner, out);
}
db::batch_status wdb::dbvisit_pages(const std::string& owner,
    const page_visitor& visit) {
    return dbs_.empty() ? batch_status::error :
        dbs_[locate(owner)]->visit_pages(owner, visit);
}
} // namespace pkv
#endif
