#include "stdafx.h"
#include "transaction_workspace.h"
#include "redis_handler.h"
#include "common/transaction_database.h"
#include "coder/record_codec.h"
#include "db/database_pool.h"
#include "db/page_workspace.h"

namespace pkv {
transaction_workspace::transaction_workspace(redis_handler& handler,
    const std::vector<std::vector<std::string>>& commands) : handler_(handler) {
    size_t selected = handler_.selected_;
    std::vector<const std::string*> legacy;
    std::function<bool(const std::vector<std::string>&)> allowed;
    if (!handler.identity_->unrestricted() || handler.identity_->revoked())
        allowed = [&](const std::vector<std::string>& args) {
            std::vector<const std::string*> borrowed;
            for (const auto& arg : args) borrowed.push_back(&arg);
            return handler.permissions().check(*handler.identity_,
                borrowed).allowed();
        };
    if (!transaction_database(commands, selected, handler_.database_count(),
        var_cfg_cluster_mode, selected, legacy, allowed)) return;
    auto source = handler_.databases_->get(selected);
    if (!source || !source->supports_workspace() ||
        !source->allows_page_writes()) return;
    std::sort(legacy.begin(), legacy.end(),
        [](const std::string* a, const std::string* b) { return *a < *b; });
    legacy.erase(std::unique(legacy.begin(), legacy.end(),
        [](const std::string* a, const std::string* b) { return *a == *b; }),
        legacy.end());
    for (const auto* key : legacy) {
        std::string value;
        if (source->lookup(*key, value) != db::lookup_status::found) continue;
        std::unique_ptr<record_view> view;
        if (source->value_codec().decode_stored(value, view) !=
            codec_status::ok) continue;
        if (!view->paged_payload() && !view->native_importable()) return;
    }
    auto workspace = std::make_shared<page_workspace>(source);
    handler_.workspace_database_ = selected;
    handler_.workspace_source_ = std::move(source);
    handler_.workspace_ = std::move(workspace);
    if (handler_.selected_ == selected) handler_.db_ = handler_.workspace_;
}
transaction_workspace::~transaction_workspace() {
    if (handler_.db_ == handler_.workspace_)
        handler_.db_ = handler_.workspace_source_;
    handler_.workspace_.reset(); handler_.workspace_source_.reset();
}
bool transaction_workspace::commit() {
    return !handler_.workspace_ ||
        handler_.workspace_->commit() == db::batch_status::ok;
}
} // namespace pkv
