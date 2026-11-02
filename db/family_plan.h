#pragma once
#include "db.h"
#include "page_transaction.h"

namespace pkv {
class database_pool;
// Final ownership of a native page family plus bounded explicit page deltas.
// family indexes another edit's PRE-GROUP family; SIZE_MAX means a fresh one.
// Each referenced original must exist and have at most one final owner.
// Erased owners use none; newly created families always use SIZE_MAX.
struct family_edit {
    unsigned database = 0;
    std::string key;
    bool expected_found = false;
    std::string expected_root;
    db::update_action action = db::update_action::write;
    std::string value;
    size_t family = SIZE_MAX;
    // Caller supplies the first eligible readiness rank from ordered history.
    size_t ready_rank = SIZE_MAX;
    std::vector<page_change> pages;
    bool maintenance_only = false, force_watch_invalidation = false;
};
// Cold unobserved memory replay primitive for future MOVE-aware EXEC plans.
// Command policy, wire negotiation and durable disk groups remain separate.
class family_plan {
public:
    enum class step { prepared, published, completed };
    using observer = std::function<void(step)>;
    // Expected roots are checked under exclusive admission and sorted key
    // locks. Logical expiry is intentionally not reinterpreted on replay.
    // Prepare every DB/family before allocation-free publication. Exceptions
    // after publication begins abort before either admission/key lock leaves.
    // Observer must not reenter DBs or mutate the borrowed input edit vector.
    static db::batch_status apply(database_pool& pool,
        const std::vector<family_edit>& edits, const observer& observe = {});
private:
    struct state;
    static db::batch_status validate(const std::vector<family_edit>& edits);
    static db::batch_status collect(database_pool& pool,
        const std::vector<family_edit>& edits, state& out);
    static db::batch_status prepare(const std::vector<family_edit>& edits,
        state& out);
};
} // namespace pkv
