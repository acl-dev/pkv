#include "stdafx.h"
#include "db.h"
#include "page_transaction.h"
#include "db_watcher.h"

namespace pkv {
struct db::paired_commit : prepare_callback {
    db& other;
    const std::vector<batch_entry>& roots;
    const std::vector<page_change>& pages;
    prepared_batch* waiting = nullptr;
    paired_commit(db& database, const std::vector<batch_entry>& r,
        const std::vector<page_change>& p) : other(database), roots(r),
        pages(p) {}
    batch_status prepared(prepared_batch& batch) override {
        if (!waiting) {
            waiting = &batch;
            return other.dbprepare_pages(roots, pages, *this);
        }
        // Both frames retain their native locks. Neither publication allocates
        // or fails. Observers and logical effects run after both frames return.
        waiting->publish(); batch.publish();
        return batch_status::ok;
    }
};
static bool changed(const std::vector<db::batch_entry>& roots,
    const std::vector<page_change>& pages) {
    if (!pages.empty()) return true;
    for (const auto& root : roots)
        if (root.action != db::update_action::keep) return true;
    return false;
}

db::batch_status db::commit_page_pair(std::vector<batch_entry>& roots,
    std::vector<page_change>& pages, db& destination,
    std::vector<batch_entry>& target_roots,
    std::vector<page_change>& target_pages) {
    const bool from_changed = changed(roots, pages);
    const bool to_changed = changed(target_roots, target_pages);
    if (!from_changed && !to_changed) return batch_status::ok;
    if ((from_changed && watchers_ && !watchers_->can_pages(roots, pages)) ||
        (to_changed && destination.watchers_ &&
            !destination.watchers_->can_pages(target_roots, target_pages)))
        return batch_status::unsupported;
    const auto ready = prepare_ready(roots, nullptr, 0, &pages);
    const auto target_ready = destination.prepare_ready(target_roots,
        nullptr, 0, &target_pages);
    if (ready_registry_) ready_registry_->prepare(ready.data(), ready.size());
    if (destination.ready_registry_) destination.ready_registry_->prepare(
        target_ready.data(), target_ready.size());
    batch_status status;
    if (std::less<db*>()(this, &destination)) {
        paired_commit next(destination, target_roots, target_pages);
        status = dbprepare_pages(roots, pages, next);
    } else {
        paired_commit next(*this, roots, pages);
        status = destination.dbprepare_pages(target_roots, target_pages, next);
    }
    if (status != batch_status::ok) return status;
    if (from_changed) publish_page_effects(roots, pages, ready);
    if (to_changed) destination.publish_page_effects(target_roots,
        target_pages, target_ready);
    // Publish effects in both DBs before calling either external observer.
    if (from_changed) observe_pages(roots, pages);
    if (to_changed) destination.observe_pages(target_roots, target_pages);
    return batch_status::ok;
}

db::batch_status db::transact_pages_with(const std::string& source,
    db& destination, const std::string& target,
    const page_copy_callback& callback) {
    // Like EXEC, this entry point cannot upgrade an existing shared admission.
    execution_guard admission(true);
    if (this == &destination) {
        if (source == target) return batch_status::aborted;
        return transact_pages({source, target}, [&](page_transaction& tx) {
            return callback(tx, tx);
        });
    }
    if (!dbsupports_pages() || !destination.dbsupports_pages() ||
        !dbsupports_prepare() || !destination.dbsupports_prepare())
        return batch_status::unsupported;
    try {
        const bool source_first = std::less<db*>()(this, &destination);
        auto& first = source_first ? key_mutex(source) :
            destination.key_mutex(target);
        auto& second = source_first ? destination.key_mutex(target) :
            key_mutex(source);
        if (!first.lock()) return batch_status::error;
        std::unique_lock<acl::fiber_mutex> first_guard(first, std::adopt_lock);
        if (!second.lock()) return batch_status::error;
        std::unique_lock<acl::fiber_mutex> second_guard(second,
            std::adopt_lock);
        std::string from, to;
        const auto source_status = dblookup(source, from);
        const auto target_status = destination.dblookup(target, to);
        if (source_status == lookup_status::error ||
            target_status == lookup_status::error) return batch_status::error;
        std::vector<batch_entry> roots, target_roots;
        roots.emplace_back(source, source_status == lookup_status::found,
            std::move(from));
        target_roots.emplace_back(target, target_status == lookup_status::found,
            std::move(to));
        page_transaction from_view(*this, batch_view(roots.data(), 1));
        page_transaction to_view(destination,
            batch_view(target_roots.data(), 1));
        const bool accepted = callback(from_view, to_view);
        if (from_view.storage_error_ || to_view.storage_error_)
            return batch_status::error;
        if (!accepted || !from_view.valid_changes() ||
            !to_view.valid_changes()) return batch_status::aborted;
        auto pages = from_view.release_changes();
        auto target_pages = to_view.release_changes();
        return commit_page_pair(roots, pages, destination, target_roots,
            target_pages);
    } catch (const std::bad_alloc&) { return batch_status::error; }
}
} // namespace pkv
