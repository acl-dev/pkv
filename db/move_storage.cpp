#include "stdafx.h"
#include "move_storage.h"
#include "move_access.h"
#include "move_journal.h"
#include <limits>

namespace pkv {
static void observe(const move_storage::callbacks& actions,
    move_storage::step at) {
    if (actions.observe) actions.observe(at);
}
db::batch_status move_storage::run(database_pool& pool, move_journal& journal,
    size_t source, size_t target, const std::string& owner,
    const callbacks& actions, bool& moved) {
    moved = false;
    if (!actions.prepare || !actions.complete ||
        source > std::numeric_limits<uint32_t>::max() ||
        target > std::numeric_limits<uint32_t>::max())
        return db::batch_status::error;
    try {
        move_decision decision;
        decision.source = static_cast<uint32_t>(source);
        decision.target = static_cast<uint32_t>(target);
        decision.owner = owner;
        move_access access(pool, source, target, decision.owner);
        if (access.status_ != db::batch_status::ok) return access.status_;
        if (journal.state() != move_journal::status::empty ||
            journal.last_serial() == std::numeric_limits<uint64_t>::max())
            return db::batch_status::error;
        decision.serial = journal.last_serial() + 1;
        if (!journal.can_decide(decision) || !access.load(decision.owner))
            return db::batch_status::error;
        const roots view{access.from_status_ == db::lookup_status::found,
            access.to_status_ == db::lookup_status::found, access.from_,
            access.to_, decision.owner};
        const auto selected = actions.prepare(view);
        if (selected == choice::skip) return db::batch_status::ok;
        if (selected != choice::proceed || !view.source_found)
            return db::batch_status::error;
        observe(actions, step::validated);
        if (access.source_->dbsync_key(decision.owner) != db::sync_status::ok)
            return db::batch_status::error;
        observe(actions, step::source_synced);
        move_access::decision_guard pending;
        if (!journal.decide(decision)) std::abort();
        observe(actions, step::decided);
        move_recovery::apply_locked(access, decision.owner, actions.apply,
            true);
        actions.complete();
        if (!journal.complete(decision.serial)) std::abort();
        observe(actions, step::completed);
        pending.completed = true; moved = true;
        return db::batch_status::ok;
    } catch (...) { return db::batch_status::error; }
}
} // namespace pkv
