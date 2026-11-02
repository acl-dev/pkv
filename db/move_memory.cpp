#include "stdafx.h"
#include "move_memory.h"
#include "move_access.h"
#include "page_transaction.h"

namespace pkv {
static void observe(const move_memory::callbacks& actions,
    move_memory::step at) {
    if (actions.observe) actions.observe(at);
}
struct move_memory::pair : db::prepare_callback {
    db& next;
    const std::vector<db::batch_entry>& next_roots;
    const callbacks& actions;
    bool source_first;
    bool& published;
    db::prepared_batch* waiting = nullptr;
    pair(db& database, const std::vector<db::batch_entry>& roots,
        const callbacks& calls, bool first, bool& started)
    : next(database), next_roots(roots), actions(calls),
        source_first(first), published(started) {}
    db::batch_status prepared(db::prepared_batch& batch) override {
        if (!waiting) {
            waiting = &batch;
            return next.dbprepare_pages(next_roots, {}, *this);
        }
        auto& source = source_first ? *waiting : batch;
        auto& target = source_first ? batch : *waiting;
        auto handoff = source.prepare_family(target);
        if (!handoff) return db::batch_status::error;
        observe(actions, step::prepared);
        published = true;
        handoff->publish(); waiting->publish(); batch.publish();
        observe(actions, step::published);
        return db::batch_status::ok;
    }
};
db::batch_status move_memory::run(database_pool& pool, size_t source,
    size_t target, const std::string& owner, const callbacks& actions,
    bool& moved) {
    moved = false;
    if (!actions.prepare || !actions.complete) return db::batch_status::error;
    try {
        const std::string key(owner);
        move_access access(pool, source, target, key,
            move_access::kind::memory);
        if (access.status_ != db::batch_status::ok) return access.status_;
        if (!access.load(key)) return db::batch_status::error;
        const roots view{access.from_status_ == db::lookup_status::found,
            access.to_status_ == db::lookup_status::found, access.from_,
            access.to_, key};
        const auto selected = actions.prepare(view);
        if (selected == choice::skip) return db::batch_status::ok;
        if (selected != choice::proceed || !view.source_found)
            return db::batch_status::error;
        observe(actions, step::validated);
        std::vector<db::batch_entry> from, to;
        from.emplace_back(key, true, "");
        from[0].action = db::update_action::erase;
        to.emplace_back(key, view.target_found, view.source);
        to[0].action = db::update_action::write; to[0].clear_pages = true;
        const auto ready = access.source_->prepare_ready(from);
        const auto target_ready = access.target_->prepare_ready(to);
        if (access.source_->ready_registry_)
            access.source_->ready_registry_->prepare(ready.data(),
                ready.size());
        if (access.target_->ready_registry_)
            access.target_->ready_registry_->prepare(target_ready.data(),
                target_ready.size());
        struct publication_guard {
            bool started = false, complete = false;
            ~publication_guard() { if (started && !complete) std::abort(); }
        } guard;
        const bool first = std::less<db*>()(access.source_.get(),
            access.target_.get());
        pair next(first ? *access.target_ : *access.source_, first ? to : from,
            actions, first, guard.started);
        const auto status = (first ? access.source_ : access.target_)->
            dbprepare_pages(first ? from : to, {}, next);
        if (status != db::batch_status::ok) return status;
        access.source_->publish_page_effects(from, {}, ready);
        access.target_->publish_page_effects(to, {}, target_ready);
        actions.complete(); observe(actions, step::completed);
        guard.complete = true; moved = true;
        return db::batch_status::ok;
    } catch (...) { return db::batch_status::error; }
}
} // namespace pkv
