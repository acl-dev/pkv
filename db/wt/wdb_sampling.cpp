#include "stdafx.h"
#ifdef HAS_WT
#include "wdb.h"
#include "wt.h"

namespace pkv {
db::scan_status wdb::dbvisit_keys(const key_visitor& visit) {
    bool proceed = true;
    for (const auto& db : dbs_) {
        const auto status = db->visit_keys([&](const std::string& key) {
            proceed = visit(key); return proceed;
        });
        if (status != scan_status::ok || !proceed) return status;
    }
    return scan_status::ok;
}
} // namespace pkv
#endif
