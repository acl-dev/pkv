#include "stdafx.h"
#include "move_recovery.h"
#include "move_journal.h"
#include "move_access.h"
#include "page_transaction.h"
#include <cstdlib>

namespace pkv {
void move_recovery::fail(const char* reason) {
    logger_error("MOVE startup recovery failed: %s", reason);
    std::abort();
}
void move_recovery::notify(const observer& observe, step point) {
    if (observe) observe(point);
}
void move_recovery::fence(db& database, const std::string& owner) {
    if (database.dbsync_key(owner) != db::sync_status::ok)
        fail("native WAL fence");
}
void move_recovery::copy(db& source, db& target, const std::string& owner,
    const std::string& bytes, bool target_found, const observer& observe) {
    std::vector<db::batch_entry> roots;
    roots.emplace_back(owner, target_found, "");
    auto& root = roots.front(); root.action = db::update_action::erase;
    if (target.dbapply(roots) != db::batch_status::ok) fail("clear target");
    notify(observe, step::target_cleared);
    std::vector<page_change> pages;
    pages.emplace_back(owner, 1, "", db::update_action::write);
    const auto status = source.dbvisit_pages(owner,
        [&](uint64_t id, const std::string& page) {
            if (!id || page.size() > page_transaction::max_value_bytes)
                return false;
            pages.front().id = id; pages.front().value = page;
            if (target.dbapply_pages({}, pages) != db::batch_status::ok)
                return false;
            notify(observe, step::page_copied); return true;
        });
    if (status != db::batch_status::ok) fail("copy private pages");
    roots.clear(); roots.emplace_back(owner, false, bytes);
    roots.front().action = db::update_action::write;
    if (target.dbapply_pages(roots, {}) != db::batch_status::ok)
        fail("publish target root");
    notify(observe, step::target_published);
}
void move_recovery::apply_locked(move_access& access,
    const std::string& owner, const observer& observe, bool source_synced) {
    auto& source = *access.source_;
    auto& target = *access.target_;
    const bool present = access.from_status_ == db::lookup_status::found;
    if (!present && access.to_status_ != db::lookup_status::found)
        fail("both roots missing");
    if (!source_synced) fence(source, owner);
    notify(observe, step::source_synced);
    if (present) copy(source, target, owner, access.from_,
        access.to_status_ == db::lookup_status::found, observe);
    else if (target.dbvisit_pages(owner,
        [](uint64_t, const std::string&) { return true; }) !=
        db::batch_status::ok) fail("surviving target pages");
    fence(target, owner); notify(observe, step::target_synced);
    if (present) {
        std::vector<db::batch_entry> roots;
        roots.emplace_back(owner, true, "");
        roots.front().action = db::update_action::erase;
        if (source.dbapply(roots) != db::batch_status::ok)
            fail("erase source");
        notify(observe, step::source_erased);
    }
    fence(source, owner); notify(observe, step::source_synced_again);
}
void move_recovery::run(database_pool& pool, move_journal& journal,
    const observer& observe) {
    if (journal.state() == move_journal::status::empty) return;
    try {
        if (!journal.pending()) fail("journal not recoverable");
        const move_decision decision(*journal.pending());
        move_access access(pool, decision.source, decision.target,
            decision.owner);
        if (access.status_ != db::batch_status::ok)
            fail("open/lock native DBs");
        move_access::decision_guard pending;
        if (!access.load(decision.owner)) fail("root lookup");
        apply_locked(access, decision.owner, observe, false);
        if (!journal.complete(decision.serial)) fail("clear durable decision");
        notify(observe, step::completed); pending.completed = true;
    } catch (...) { fail("exception during pending decision"); }
}
} // namespace pkv
