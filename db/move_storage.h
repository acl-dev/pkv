#pragma once
#include "db.h"
#include "move_recovery.h"

namespace pkv {
class database_pool;
class move_journal;
// Physical phase of a future online MOVE adapter. This is not command policy
// or a complete live operation. An adapter must validate type/expiry/target
// absence and preallocate logical effects/publication in prepare, then publish
// those effects in complete under the same locks. No callback may reenter DBs.
class move_storage {
public:
    enum class choice { proceed, skip, error };
    enum class step { validated, source_synced, decided, completed };
    struct roots {
        bool source_found, target_found;
        const std::string& source;
        const std::string& target;
        const std::string& owner;
    };
    struct callbacks {
        std::function<choice(const roots&)> prepare;
        std::function<void()> complete;
        std::function<void(step)> observe;
        move_recovery::observer apply;
    };
    // The owned key and root snapshots live through both callbacks.
    // Both callbacks are mandatory. ok with moved=false means no decision.
    // Before decision: errors return with unchanged values. Once publication
    // is attempted: any failure aborts before either key lock is released.
    // The caller binds this journal to this pool and serializes other access
    // through the same exclusive execution admission. Callbacks must not
    // mutate the journal. Root bytes, including expiry, remain opaque.
    static db::batch_status run(database_pool& pool, move_journal& journal,
        size_t source, size_t target, const std::string& owner,
        const callbacks& actions, bool& moved);
};
} // namespace pkv
