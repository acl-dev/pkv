#include "stdafx.h"
#include "page_workspace.h"
#include "key_count.h"
#include <stdexcept>

namespace pkv {
page_workspace::page_workspace(shared_db source, size_t bytes, size_t records)
: db(true), admission_(true), source_(std::move(source)),
  byte_limit_(std::min(bytes, page_batch::max_bytes)),
  record_limit_(std::min(records, page_batch::max_records)) {
    if (!source_ || !source_->dbsupports_pages() ||
        dynamic_cast<page_workspace*>(source_.get()))
        throw std::invalid_argument("workspace requires a native source DB");
    codec_ = source_->codec_;
}
page_workspace::~page_workspace() = default;

void page_workspace::discard() {
    owners_.clear(); batches_.clear(); root_count_delta_ = 0;
    bytes_ = 56; records_ = 2; finished_ = true;
}
db::batch_status page_workspace::commit() {
    if (finished_) return batch_status::aborted;
    if (batches_.empty()) { discard(); return batch_status::ok; }
    try {
        std::vector<page_update> updates;
        updates.reserve(batches_.size());
        for (const auto& entry : batches_) {
            updates.emplace_back(entry.batch->roots(), entry.batch->pages());
            updates.back().ready_types = entry.ready_types;
        }
        std::vector<batch_entry> roots;
        std::vector<page_change> pages;
        prepare_commit(roots, pages);
        const auto status = source_->commit_workspace(roots, pages, updates);
        if (status == batch_status::ok) discard();
        return status;
    } catch (const std::bad_alloc&) { return batch_status::error; }
}

bool page_workspace::dbget(const std::string& key, std::string& out) {
    return dblookup(key, out) == lookup_status::found;
}
db::lookup_status page_workspace::dblookup(const std::string& key,
    std::string& out) {
    const auto found = owners_.find(key);
    if (found != owners_.end() && found->second.root) {
        const auto& root = *found->second.root;
        if (root.action == update_action::erase) return lookup_status::missing;
        out = root.value; return lookup_status::found;
    }
    return source_->dblookup(key, out);
}
db::lookup_status page_workspace::dblookup_page(const std::string& owner,
    uint64_t id, std::string& out) {
    const auto found = owners_.find(owner);
    if (found != owners_.end()) {
        const auto& state = found->second;
        if (state.root && state.root->action == update_action::erase)
            return lookup_status::missing;
        const auto page = state.pages.find(id);
        if (page != state.pages.end() && page->second.page &&
            page->second.generation == state.generation) {
            if (page->second.page->action == update_action::erase)
                return lookup_status::missing;
            out = page->second.page->value; return lookup_status::found;
        }
        if (state.reset) return lookup_status::missing;
    }
    return source_->dblookup_page(owner, id, out);
}

bool page_workspace::dbset(const std::string& key, const std::string& value) {
    std::string previous;
    const auto found = dblookup(key, previous);
    if (found == lookup_status::error) return false;
    std::vector<batch_entry> roots;
    roots.emplace_back(key, found == lookup_status::found, value);
    roots.back().action = update_action::write;
    roots.back().clear_pages = true;
    return dbapply_pages(roots, {}) == batch_status::ok;
}
bool page_workspace::dbdel(const std::string& key) {
    std::string previous;
    if (dblookup(key, previous) != lookup_status::found) return false;
    std::vector<batch_entry> roots;
    roots.emplace_back(key, true, "");
    roots.back().action = update_action::erase;
    return dbapply_pages(roots, {}) == batch_status::ok;
}
db::batch_status page_workspace::dbapply(
    const std::vector<batch_entry>& entries) {
    auto roots = entries;
    for (auto& root : roots)
        if (root.action == update_action::write) root.clear_pages = true;
    return dbapply_pages(roots, {});
}
db::batch_status page_workspace::dbapply_pages(
    const std::vector<batch_entry>& roots,
    const std::vector<page_change>& pages) {
    if (finished_) return batch_status::aborted;
    return stage(page_batch::capture(0, roots, pages));
}
size_t page_workspace::dbsize() const { return source_->dbsize(); }

db::count_status page_workspace::dbkey_count(uint64_t& out) {
    uint64_t count;
    const auto status = source_->dbkey_count(count);
    if (status != count_status::ok) return status;
    if (root_count_delta_ < 0) {
        const auto removed = static_cast<uint64_t>(-root_count_delta_);
        if (count < removed) return count_status::error;
        count -= removed;
    } else if (!add_key_count(static_cast<uint64_t>(root_count_delta_), count))
        return count_status::error;
    out = count; return count_status::ok;
}

db::scan_status page_workspace::dbscan_keys(unsigned long long cursor,
    size_t count, std::vector<std::string>& keys, unsigned long long& next) {
    const auto status = source_->dbscan_keys(cursor, count, keys, next);
    if (status != scan_status::ok) return status;
    try {
        keys.erase(std::remove_if(keys.begin(), keys.end(),
            [&](const std::string& key) {
                const auto found = owners_.find(key);
                return found != owners_.end() && found->second.root &&
                    found->second.root->action == update_action::erase;
            }), keys.end());
        // COUNT is a hint. Include staged keys on the first native page;
        // later native pages may repeat an overwritten key, as SCAN permits.
        if (!cursor) {
            for (const auto& owner : owners_)
                if (owner.second.root &&
                    owner.second.root->action == update_action::write)
                    keys.push_back(owner.first);
            std::sort(keys.begin(), keys.end());
            keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
        }
        return scan_status::ok;
    } catch (const std::bad_alloc&) {
        keys.clear(); next = 0; return scan_status::error;
    }
}
} // namespace pkv
