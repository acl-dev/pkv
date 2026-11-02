#include "stdafx.h"
#ifdef HAS_WT
#include "wt.h"
#include "wt_sess.h"
#include "db/disk_address.h"

namespace pkv {
namespace {
struct bootstrap_session {
    WT_SESSION* session = nullptr;
    bool transaction = false;
    ~bootstrap_session() {
        if (transaction) session->rollback_transaction(session, nullptr);
        if (session) session->close(session, nullptr);
    }
    bool begin() {
        if (session->begin_transaction(session, "isolation=snapshot"))
            return false;
        transaction = true; return true;
    }
    bool commit() {
        const int result = session->commit_transaction(session, "sync=on");
        transaction = false; return result == 0;
    }
    bool cursor(const char* table, WT_CURSOR** out) {
        return session->open_cursor(session, table, nullptr, nullptr, out) == 0;
    }
};
bool import_legacy(bootstrap_session& context, WT_CURSOR* roots) {
    WT_CURSOR* metadata = nullptr;
    if (!context.cursor("metadata:", &metadata)) return false;
    metadata->set_key(metadata, "table:access");
    const int found = metadata->search(metadata);
    metadata->reset(metadata);
    if (found == WT_NOTFOUND) return true;
    if (found) return false;
    WT_CURSOR* legacy = nullptr;
    if (!context.cursor("table:access", &legacy) ||
        std::strcmp(legacy->key_format, "S") ||
        std::strcmp(legacy->value_format, "S")) return false;
    int result;
    while ((result = legacy->next(legacy)) == 0) {
        const char *key, *value;
        if (legacy->get_key(legacy, &key) || legacy->get_value(legacy, &value))
            return false;
        const auto address = std::string(1, '\1') + key;
        wt_sess::key(roots, address);
        const int exists = roots->search(roots);
        roots->reset(roots);
        if (exists == WT_NOTFOUND) {
            if (wt_sess::put(roots, address, value)) return false;
        } else if (exists) return false;
    }
    legacy->reset(legacy);
    return result == WT_NOTFOUND;
}
bool rebuild_index(bootstrap_session& context, WT_CURSOR* roots,
    WT_CURSOR* index) {
    if (!context.begin()) return false;
    if (context.session->truncate(context.session, "table:pkv_scan_v1",
        nullptr, nullptr, nullptr)) return false;
    if (!context.commit()) return false;
    roots->reset(roots);
    int result;
    while ((result = roots->next(roots)) == 0) {
        std::string key;
        if (wt_sess::value(roots, key, true) || key.empty() || key[0] != '\1')
            return false;
        key.erase(0, 1);
        if (wt_sess::put(index, disk_address::index(key), "")) return false;
    }
    roots->reset(roots);
    return result == WT_NOTFOUND;
}
}

bool wt::bootstrap() {
    bootstrap_session context;
    if (db_->open_session(db_, nullptr, nullptr, &context.session))
        return false;
    WT_CURSOR *roots = nullptr, *index = nullptr, *metadata = nullptr;
    const char* config = "key_format=u,value_format=u,log=(enabled=true),"
        "leaf_page_max=16KB";
    for (const auto* table : {"table:pkv_roots_v1", "table:pkv_pages_v1",
        "table:pkv_scan_v1", "table:pkv_meta_v1"}) {
        if (context.session->create(context.session, table, config))
            return false;
        WT_CURSOR* cursor = nullptr;
        if (!context.cursor(table, &cursor) ||
            std::strcmp(cursor->key_format, "u") ||
            std::strcmp(cursor->value_format, "u")) return false;
        cursor->close(cursor);
    }
    if (!context.cursor("table:pkv_roots_v1", &roots) ||
        !context.cursor("table:pkv_scan_v1", &index) ||
        !context.cursor("table:pkv_meta_v1", &metadata)) return false;
    const std::string schema_key = "schema";
    wt_sess::key(metadata, schema_key);
    const int found = metadata->search(metadata);
    if (!found) {
        std::string schema;
        return !wt_sess::value(metadata, schema) && schema == "binary-v1";
    }
    metadata->reset(metadata);
    if (found != WT_NOTFOUND || !import_legacy(context, roots) ||
        !rebuild_index(context, roots, index) || !context.begin()) return false;
    if (wt_sess::put(metadata, "schema", "binary-v1")) return false;
    return context.commit();
}

} // namespace pkv
#endif
