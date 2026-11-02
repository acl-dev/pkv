#include "stdafx.h"
#include "page_workspace.h"

namespace pkv {
db::scan_status page_workspace::dbvisit_keys(const key_visitor& visit) {
    bool proceed = true;
    const auto status = source_->dbvisit_keys([&](const std::string& key) {
        const auto owner = owners_.find(key);
        if (owner != owners_.end() && owner->second.root &&
            owner->second.root->action != update_action::keep) return true;
        proceed = visit(key); return proceed;
    });
    if (status != scan_status::ok || !proceed) return status;
    // Overwritten base keys were excluded above; every staged write appears
    // once, including keys that would repeat on the ordinary SCAN overlay.
    for (const auto& owner : owners_)
        if (owner.second.root &&
            owner.second.root->action == update_action::write &&
            !visit(owner.first)) break;
    return scan_status::ok;
}
} // namespace pkv
