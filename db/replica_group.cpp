#include "stdafx.h"
#include "replica_group.h"
#include "database_pool.h"
#include "page_workspace.h"
#include "rpc/exec_batch.h"

namespace pkv {
struct replica_group::plan {
    shared_db database;
    std::unique_ptr<page_workspace> view;
    std::vector<page_update> updates;
    std::vector<db::batch_entry> roots;
    std::vector<page_change> pages;
    std::vector<key_ready_change> ready;
    // Destroy the frame before its borrowed inputs and database. This also
    // rolls back prepared roots/pages before the outer key guards unlock.
    std::unique_ptr<db::prepared_batch> frame;
};
db::batch_status replica_group::collect(database_pool& pool,
    const exec_batch& group, plans& out, std::vector<plan*>& ordered) {
    for (const auto& batch : group.batches()) {
        if (batch->database() >= pool.size()) return db::batch_status::aborted;
        out.emplace(batch->database(), nullptr);
    }
    for (auto& entry : out) {
        std::unique_ptr<plan> item(new plan);
        item->database = pool.get(entry.first);
        if (!item->database) return db::batch_status::error;
        if (!item->database->dbsupports_owned_prepare())
            return db::batch_status::unsupported;
        if (!item->database->opened_) return db::batch_status::error;
        entry.second = std::move(item);
        ordered.push_back(entry.second.get());
    }
    std::sort(ordered.begin(), ordered.end(), [](const plan* a, const plan* b) {
        return std::less<db*>()(a->database.get(), b->database.get());
    });
    for (size_t i = 1; i < ordered.size(); ++i)
        if (ordered[i - 1]->database == ordered[i]->database)
            return db::batch_status::unsupported;
    for (auto* item : ordered)
        item->view.reset(new page_workspace(item->database));
    return db::batch_status::ok;
}
db::batch_status replica_group::stage(const exec_batch& group,
    plans& databases) {
    // Validate every prefix against that DB's private overlay. A later root
    // write must not rescue an earlier page write to a missing owner.
    for (const auto& batch : group.batches()) {
        auto& item = *databases.find(batch->database())->second;
        const auto status = item.view->replica_apply_pages(batch->roots(),
            batch->pages());
        if (status != db::batch_status::ok) return status;
        item.updates.emplace_back(batch->roots(), batch->pages());
    }
    return db::batch_status::ok;
}
void replica_group::prepare(plan& item) {
    item.view->prepare_commit(item.roots, item.pages);
    item.ready = item.database->prepare_ready(item.roots,
        item.updates.data(), item.updates.size(), &item.pages);
    if (item.database->ready_registry_)
        item.database->ready_registry_->prepare(item.ready.data(),
            item.ready.size());
}
db::batch_status replica_group::apply(database_pool& pool,
    const exec_batch& group) {
    execution_guard admission(true);
    try {
        // Plans must roll back and release native locks before key guards.
        std::vector<std::unique_lock<acl::fiber_mutex>> guards;
        plans databases;
        std::vector<plan*> ordered;
        auto status = collect(pool, group, databases, ordered);
        if (status != db::batch_status::ok) return status;
        std::vector<acl::fiber_mutex*> mutexes;
        for (const auto& batch : group.batches()) {
            auto& database = *databases.find(batch->database())->second->
                database;
            for (const auto& root : batch->roots())
                mutexes.push_back(&database.key_mutex(root.key));
        }
        std::sort(mutexes.begin(), mutexes.end(),
            std::less<acl::fiber_mutex*>());
        mutexes.erase(std::unique(mutexes.begin(), mutexes.end()),
            mutexes.end());
        guards.reserve(mutexes.size());
        for (auto* mutex : mutexes) {
            if (!mutex->lock()) return db::batch_status::error;
            guards.emplace_back(*mutex, std::adopt_lock);
        }
        status = stage(group, databases);
        if (status != db::batch_status::ok) return status;
        // Logical preparation may read native roots. Finish it before any
        // native frame retains shard/index locks, then prepare iteratively.
        for (auto* item : ordered) prepare(*item);
        for (auto* item : ordered) {
            status = item->database->dbprepare_owned_pages(item->roots,
                item->pages, item->frame);
            if (status != db::batch_status::ok) return status;
            if (!item->frame) return db::batch_status::error;
        }
        struct publication_guard {
            bool complete = false;
            ~publication_guard() { if (!complete) std::abort(); }
        } guard;
        for (auto* item : ordered) item->frame->publish();
        for (auto* item : ordered)
            item->database->publish_page_effects(item->roots, item->pages,
                item->ready);
        guard.complete = true;
        return db::batch_status::ok;
    } catch (...) { return db::batch_status::error; }
}
} // namespace pkv
