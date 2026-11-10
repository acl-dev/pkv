#include "page_transaction.h"
#include "map_store.h"

namespace pkv {
db::lookup_status page_transaction::inspect_bound(db::batch_entry* root,
    uint64_t id, std::string& scratch, db::page_inspector visit,
    void* context) {
    if (!visit) {
        failed_ = true;
        return db::lookup_status::error;
    }
    return inspect_bound_inline(root, id, scratch, visit, context);
}

bool map_store::inspect(uint64_t id, std::string& scratch,
    db::page_inspector inspect_value, void* context) {
    if (!id || id > last_id()) return fail(status::invalid);
    const auto result = transaction_.inspect_bound(entry_, id, scratch,
        inspect_value, context);
    if (result == db::lookup_status::found) return true;
    return fail(result == db::lookup_status::error ? status::storage :
        status::invalid);
}
} // namespace pkv
