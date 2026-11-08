#pragma once
#include "coder/map_page.h"

namespace pkv {
namespace detail {

// One bounded byte buffer per worker. A live reader excludes nested calls
// and other fibers; they retain independent, ordinary string storage.
class node_read_workspace {
    struct slot {
        bool busy = false;
        std::string bytes;
    };
    static slot& local() {
        static thread_local slot value;
        return value;
    }
    slot* cached_ = nullptr;
    bool acquired_ = false;
    std::string fallback_;
public:
    node_read_workspace() = default;
    node_read_workspace(const node_read_workspace&) = delete;
    node_read_workspace& operator=(const node_read_workspace&) = delete;
    ~node_read_workspace() {
        if (!cached_) return;
        if (cached_->bytes.capacity() > map_page_codec::max_page_bytes)
            std::string().swap(cached_->bytes);
        else cached_->bytes.clear();
        cached_->busy = false;
    }
    std::string& buffer() {
        if (!acquired_) {
            acquired_ = true;
            auto& value = local();
            if (!value.busy) { value.busy = true; cached_ = &value; }
        }
        return cached_ ? cached_->bytes : fallback_;
    }
    static size_t retained_capacity() { return local().bytes.capacity(); }
};

} // namespace detail
} // namespace pkv
