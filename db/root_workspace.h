#pragma once
#include "page_transaction.h"
#include "node_write_workspace.h"

namespace pkv {
namespace detail {

// ACL fibers run on their worker's scheduler. A live lease excludes every
// other fiber and nested transaction on that worker from this one-slot cache.
class root_workspace {
    struct slot {
        bool busy = false;
        std::vector<db::batch_entry> roots;
        std::vector<page_change> pages;
        std::string bytes;
        std::string page_owner;
    };
    static slot& local() {
        static thread_local slot value;
        return value;
    }
    slot* cached_ = nullptr;
    std::vector<db::batch_entry> fallback_;
    std::vector<page_change> fallback_pages_;
public:
    explicit root_workspace(size_t count) {
        if (count == 1) acquire();
    }
    void acquire() {
        if (cached_) return;
        auto& value = local();
        if (value.busy) return;
        value.busy = true; cached_ = &value;
    }
    root_workspace(const root_workspace&) = delete;
    root_workspace& operator=(const root_workspace&) = delete;
    ~root_workspace() {
        // Commit and all borrowed backend/observer callbacks have finished.
        for (auto& page : pages())
            node_write_workspace::recycle(page.value);
        if (!cached_) return;
        if (!cached_->pages.empty()) {
            auto& owner = cached_->pages.front().owner;
            if (owner.size() > 32 && owner.capacity() <= 1024) {
                owner.clear();
                cached_->page_owner.swap(owner);
            }
        }
        // Retain capacity only, never live page owners, values or actions.
        cached_->pages.clear();
        if (cached_->pages.capacity() > 128)
            std::vector<page_change>().swap(cached_->pages);
        if (!cached_->roots.empty())
            retain_buffer(cached_->roots.front().value);
        // Destroy const key/found and every flag; retain only bounded capacity.
        cached_->roots.clear();
        if (cached_->bytes.capacity() > page_transaction::max_value_bytes)
            std::string().swap(cached_->bytes);
        else
            cached_->bytes.clear();
        cached_->busy = false;
    }
    std::vector<db::batch_entry>& roots() {
        return cached_ ? cached_->roots : fallback_;
    }
    std::vector<page_change>& pages() {
        return cached_ ? cached_->pages : fallback_pages_;
    }
    std::string* page_owner_buffer() {
        return cached_ ? &cached_->page_owner : nullptr;
    }
    void read_buffer(std::string& out) {
        if (cached_) out.swap(cached_->bytes);
        out.clear();
    }
    std::string& borrow_buffer(std::string& fallback) {
        return cached_ ? cached_->bytes : fallback;
    }
    void retain_buffer(std::string& value) {
        if (cached_ && value.capacity() <= page_transaction::max_value_bytes)
            cached_->bytes.swap(value);
    }
    static size_t retained_value_capacity() {
        return local().bytes.capacity();
    }
    static size_t retained_page_capacity() {
        return local().pages.capacity();
    }
};

} // namespace detail
} // namespace pkv
