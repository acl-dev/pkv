#pragma once
#include "move_storage.h"

namespace pkv {
class database_pool;
// Volatile ownership phase for matching native memory engines. This is not a
// public command. Local WATCH/expiry/readiness effects are published in both
// DBs; complete must supply paired observer/replication publication. It must
// not emit root-only page deltas for a transferred nonempty family.
class move_memory {
public:
    using roots = move_storage::roots;
    using choice = move_storage::choice;
    enum class step { validated, prepared, published, completed };
    struct callbacks {
        std::function<choice(const roots&)> prepare;
        std::function<void()> complete;
        std::function<void(step)> observe;
    };
    // Preparation errors roll both engines back. Publication never allocates
    // or fails. After publication, a callback failure aborts before unlocking.
    // Same-DB, mixed-engine and workspace transfers decline before callbacks.
    static db::batch_status run(database_pool& pool, size_t source,
        size_t target, const std::string& owner, const callbacks& actions,
        bool& moved);
private:
    struct pair;
};
} // namespace pkv
