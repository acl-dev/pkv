#pragma once
#include "coder/map_page.h"
#include <memory>

namespace pkv {
namespace detail {

// Owned by a single logical-key lock scope and invalidated by page writes.
struct map_group_cache {
    struct entry {
        uint64_t id = 0, elements = 0, last_id = 0;
        map_group group;
    };
    entry groups[17];
    unsigned validated_groups = 0;
};

// Scratch ownership lasts for the entire MapStore lifetime. Nested calls and
// other fibers on this worker use independent storage while the slot is busy.
// Every public operation still reads and validates its transaction's pages.
class map_index_workspace {
public:
    struct state {
        map_group top, group;
        std::unique_ptr<map_directory> directory;
        unsigned group_index = 16;
        bool top_cached = false;
    };
private:
    struct slot {
        bool busy = false;
        state data;
    };
    static slot& local() {
        static thread_local slot value;
        return value;
    }
    state* data_ = nullptr;
public:
    map_index_workspace() = default;
    map_index_workspace(const map_index_workspace&) = delete;
    map_index_workspace& operator=(const map_index_workspace&) = delete;
    ~map_index_workspace() { reset(); }
    explicit operator bool() const { return data_ != nullptr; }
    state* operator->() { return data_; }
    state& acquire() {
        if (data_) return *data_;
        auto& value = local();
        if (value.busy) data_ = new state;
        else {
            value.busy = true;
            data_ = &value.data;
            data_->group_index = 16;
            data_->top_cached = false;
        }
        return *data_;
    }
    void reset() noexcept {
        if (!data_) return;
        auto& value = local();
        if (data_ == &value.data) {
            // Never retain the optional flat directory's heap storage.
            data_->directory.reset();
            value.busy = false;
        } else delete data_;
        data_ = nullptr;
    }
};

} // namespace detail
} // namespace pkv
