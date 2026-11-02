#include "stdafx.h"
#include "db.h"

namespace pkv {
bool db::close_for_exit() {
    if (!opened_) return true;
    try {
        const bool ok = dbclose_for_exit();
        if (ok) opened_ = false;
        return ok;
    } catch (...) { return false; }
}
} // namespace pkv
