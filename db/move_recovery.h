#pragma once
#include <functional>
#include <string>

namespace pkv {
class db;
class database_pool;
class move_journal;
class move_access;
// Startup-only redo, before serving clients, expiry or replication. This is
// not command execution: it emits no logical observer/WATCH/readiness effects.
// The decision creator must have validated both keys and synced the source
// under retained locks before publishing the journal. Only native disk stores
// support this path. Root/page bytes remain opaque and bounded to 16 KiB.
class move_recovery {
public:
    enum class step { source_synced, target_cleared, page_copied,
        target_published, target_synced, source_erased, source_synced_again,
        completed };
    // A diagnostic hook runs under locks and must not access DBs or journal.
    using observer = std::function<void(step)>;
    // Empty returns without touching DBs. A failed journal, invalid pending
    // record or IO failure aborts the process. Once both key locks are taken,
    // exceptions abort before unlocking. No partial result is returned.
    static void run(database_pool& pool, move_journal& journal,
        const observer& observe = {});
private:
    friend class move_storage;
    // Caller retains both key locks and fail-stop protection. This phase does
    // not clear the journal or emit logical events. No recursive admission.
    static void apply_locked(move_access& access, const std::string& owner,
        const observer& observe, bool source_synced);
    static void copy(db& source, db& target, const std::string& owner,
        const std::string& bytes, bool target_found, const observer& observe);
    static void fence(db& database, const std::string& owner);
    static void notify(const observer& observe, step point);
    [[noreturn]] static void fail(const char* reason);
};
} // namespace pkv
