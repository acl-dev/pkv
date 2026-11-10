#pragma once
#include "db/page_transaction.h"
#include <array>
#include <cstring>
#include <vector>
#include <algorithm>

namespace pkv {
namespace detail {

inline bool needs_inline_buffer(size_t capacity, size_t size) {
    const auto inline_capacity = std::string().capacity();
    return size <= inline_capacity && capacity > inline_capacity;
}

constexpr size_t max_small_string_pages = 8;

inline bool can_adopt_small_string_family(const db::batch_entry& root,
    const std::vector<page_change>& pages) {
    return pages.size() > 1 && pages.size() <= max_small_string_pages &&
        root.found && root.clear_pages && root.action == db::update_action::write &&
        root.value.compare(0, 4, "PSR\1", 4) == 0;
}

template<class Index>
bool adopt_small_string_family(std::string& stored, Index& index,
    const db::batch_entry& root, std::vector<page_change>& pages) {
    if (pages.size() > max_small_string_pages || stored.size() != root.value.size() ||
        !index.has_small_pages(pages.size()))
        return false;
    std::array<std::string*, max_small_string_pages> targets;
    const auto inline_capacity = std::string().capacity();
    for (size_t i = 0; i < pages.size(); ++i) {
        const auto& page = pages[i];
        if (page.id != i + 1 || page.owner != root.key ||
            page.action != db::update_action::write ||
            (page.value.capacity() > inline_capacity &&
                page.value.capacity() - page.value.size() >
                    std::max<size_t>(16, page.value.size() / 4))) return false;
        targets[i] = index.get(page.id);
        if (!targets[i]) return false;
    }
    // The caller holds the owner's lock and observers do not need staged
    // payloads. All destinations were checked before the first swap.
    for (size_t i = 0; i < pages.size(); ++i) targets[i]->swap(pages[i].value);
    std::memcpy(&stored[0], root.value.data(), root.value.size());
    return true;
}

// Two inline buffers and bounded spill storage share one worker-local lease.
// Nested actors use private storage. Owned preparations opt out of the cache.
class replacement_workspace {
    using entry = std::pair<std::string*, std::string>;
    struct slot {
        bool busy = false;
        std::array<std::string, 2> bytes;
        std::vector<std::string> spill;
    };
    static slot& local() {
        static thread_local slot value;
        return value;
    }
    std::array<entry, 2> values_{};
    slot* cached_ = nullptr;
    unsigned used_ = 0;
public:
    enum : size_t { max_spill_entries = 64, max_spill_capacity = 1024 };
    explicit replacement_workspace(bool recycle) {
        if (!recycle) return;
        auto& value = local();
        if (value.busy) return;
        value.busy = true; cached_ = &value;
    }
    replacement_workspace(const replacement_workspace&) = delete;
    replacement_workspace& operator=(const replacement_workspace&) = delete;
    ~replacement_workspace() {
        if (!cached_) return;
        for (size_t i = 0; i < values_.size(); ++i) {
            if (!(used_ & (1U << i))) continue;
            auto& bytes = values_[i].second;
            if (bytes.capacity() <= page_transaction::max_value_bytes)
                cached_->bytes[i].swap(bytes);
            cached_->bytes[i].clear();
        }
        cached_->busy = false;
    }
    size_t size() const { return values_.size(); }
    entry& take(size_t index) {
        if (cached_) {
            values_[index].second.swap(cached_->bytes[index]);
            used_ |= 1U << index;
        }
        return values_[index];
    }
    void stage(size_t index, std::string& target, const std::string& value,
        bool compact = false) {
        auto& replacement = take(index);
        const auto capacity = replacement.second.capacity();
        if ((capacity < value.size() &&
                (compact || value.size() >= page_transaction::max_value_bytes / 8)) ||
            (compact && (capacity > value.size() + value.size() / 4 ||
                needs_inline_buffer(capacity, value.size())))) {
            // Sealed pages and large values should not inherit growth slack.
            std::string exact(value);
            replacement.second.swap(exact);
        } else replacement.second = value;
        replacement.first = &target;
    }
    entry& operator[](size_t index) { return values_[index]; }
    void stage_spill(std::vector<entry>& values, std::string& target,
        const std::string& value, size_t hint) {
        if (values.empty()) values.reserve(hint);
        const auto index = values.size();
        if (!cached_ || index >= max_spill_entries) {
            values.emplace_back(&target, value);
            return;
        }
        auto& buffers = cached_->spill;
        if (buffers.empty()) buffers.reserve(max_spill_entries);
        // Exact-sized replacements can occupy earlier slots without using
        // the cache. Construct the gap before indexing the next slot.
        if (buffers.size() <= index) buffers.resize(index + 1);
        std::string prepared;
        prepared.swap(buffers[index]);
        // Keep publication capacity bounded when switching payload sizes.
        if (needs_inline_buffer(prepared.capacity(), value.size()) ||
            prepared.capacity() < value.size() ||
            prepared.capacity() - value.size() > value.size() / 4 + 32) {
            std::string exact(value);
            prepared.swap(exact);
        } else prepared = value;
        values.emplace_back(&target, std::move(prepared));
    }
    void release_spill(std::vector<entry>& values) noexcept {
        if (!cached_) return;
        auto& buffers = cached_->spill;
        const auto count = std::min(values.size(), buffers.size());
        for (size_t i = 0; i < count; ++i) {
            auto& bytes = values[i].second;
            if (bytes.capacity() <= max_spill_capacity) buffers[i].swap(bytes);
            buffers[i].clear();
        }
    }
    static size_t retained_spill_entries() { return local().spill.size(); }
    static size_t retained_spill_capacity() {
        size_t total = 0;
        for (const auto& bytes : local().spill) total += bytes.capacity();
        return total;
    }
    static size_t retained_value_capacity() {
        const auto& value = local();
        return value.bytes[0].capacity() + value.bytes[1].capacity();
    }
};

constexpr size_t max_bound_zset_edits = 64;

// The logical key lock pins the owner. Prepare replacements and insert new
// pages before publishing existing edits. Insertion preserves page addresses.
template<bool Recycle, class Index>
db::batch_status commit_bound_zset_pages(std::string& stored, Index& index,
    const db::batch_entry& root, const std::vector<page_change>& pages) {
    using status = db::batch_status;
    if (stored.size() != root.value.size() || pages.size() > max_bound_zset_edits)
        return status::unsupported;
    std::array<std::string*, max_bound_zset_edits> copies;
    std::array<size_t, max_bound_zset_edits> additions;
    std::vector<std::pair<std::string*, std::string>> replacements;
    replacement_workspace workspace(Recycle);
    size_t addition_count = 0, inserted = 0;
    try {
        for (size_t i = 0; i < pages.size(); ++i) {
            const auto& page = pages[i];
            copies[i] = nullptr;
            if (page.owner != root.key) return status::unsupported;
            if (page.action == db::update_action::erase) continue;
            if (page.action != db::update_action::write) return status::unsupported;
            auto* target = index.get(page.id);
            if (!target) {
                additions[addition_count++] = i;
                continue;
            }
            if (target->size() == page.value.size() &&
                !needs_inline_buffer(target->capacity(), page.value.size()))
                copies[i] = target;
            else if (Recycle) workspace.stage_spill(replacements, *target, page.value, 4);
            else replacements.emplace_back(target, page.value);
        }
        for (; inserted < addition_count; ++inserted) {
            const auto& page = pages[additions[inserted]];
            index.emplace(page.id, page.value);
        }
    } catch (const std::bad_alloc&) {
        while (inserted) index.erase(pages[additions[--inserted]].id);
        return status::error;
    }
    for (auto& item : replacements) item.first->swap(item.second);
    workspace.release_spill(replacements);
    for (size_t i = 0; i < pages.size(); ++i)
        if (copies[i] && !pages[i].value.empty())
            std::memcpy(&(*copies[i])[0], pages[i].value.data(), pages[i].value.size());
    for (const auto& page : pages) {
        if (page.action != db::update_action::erase) continue;
        index.erase(page.id, true);
        index.reclaim_prefix();
    }
    if (!root.value.empty())
        std::memcpy(&stored[0], root.value.data(), root.value.size());
    // Promotion may relocate page strings after all prepared edits are used.
    if (addition_count) {
        index.promote_prefix(addition_count);
        index.promote_tail(pages[additions[addition_count - 1]].id);
    }
    return status::ok;
}

} // namespace detail
} // namespace pkv
