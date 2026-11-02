#include "stdafx.h"
#ifdef HAS_WT
#include "wt.h"
#include "wdb.h"

namespace pkv {
bool wdb::dbclose_for_exit() {
    bool ok = true;
    for (const auto& database : dbs_)
        if (!database->close()) ok = false;
    dbs_.clear();
    return ok;
}
} // namespace pkv
#endif
