#pragma once

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pkv {

// Inserting pages preserves string addresses. Promotion and reclamation run
// after prepared replacements no longer use page addresses.
template<bool CompactGrowth>
class mdb_page_index_impl {
    static constexpr size_t prefix_limit = 256;
    static constexpr size_t tail_limit = 64;
    static constexpr size_t tail_growth_min = tail_limit * 3 / 4;
    static constexpr size_t tail_gap_limit = 8;
    static constexpr size_t tail_release_limit = 8;
    static_assert(prefix_limit <= UINT16_MAX && tail_limit <= prefix_limit,
        "dense block sizes fit their metadata and presence bitmap");
    template<size_t Slots>
    struct page_block {
        std::bitset<Slots> present;
        uint16_t count = 0;
    };
    template<size_t Slots>
    struct fixed_pages : page_block<Slots> {
        std::array<std::string, Slots> values;
    };
    static constexpr size_t first_slots = 2;
    static constexpr size_t small_limit = 8;
    struct inline_pages {
        std::array<bool, first_slots> present{};
        uint16_t count = 0;
        std::unique_ptr<fixed_pages<small_limit - first_slots>> extension;
        std::array<std::string, first_slots> values;
    };
    // Keep only metadata in the movable directory. The separately allocated
    // array preserves page addresses and avoids padding its allocation with
    // a bitmap/count header. Its presence bitmap also supplies the count.
    struct tail_block {
        std::bitset<tail_limit> present;
        std::unique_ptr<std::array<std::string, tail_limit>> values;
    };
    static_assert(std::is_nothrow_move_constructible<tail_block>::value,
        "directory growth must move only nonthrowing page metadata");
    using block_map = std::unordered_map<uint64_t, tail_block>;
    struct dense_prefix : page_block<prefix_limit> {
        explicit dense_prefix(size_t size) {
            static_assert(std::is_nothrow_default_constructible<std::string>::value,
                "page strings must default-construct without throwing");
            if (!size) return;
            std::allocator<std::string> allocator;
            auto* storage = allocator.allocate(size);
            for (size_t i = 0; i < size; ++i)
                ::new (static_cast<void*>(storage + i)) std::string;
            values = storage;
            limit = static_cast<uint16_t>(size);
        }
        dense_prefix(const dense_prefix&) = delete;
        dense_prefix& operator=(const dense_prefix&) = delete;
        ~dense_prefix() { release_values(); }
        void release_values() noexcept {
            auto* storage = values;
            const auto slots = limit;
            values = nullptr; limit = 0;
            if (!storage) return;
            std::allocator<std::string> allocator;
            for (size_t i = slots; i; --i)
                std::allocator_traits<std::allocator<std::string>>::destroy(
                    allocator, storage + i - 1);
            allocator.deallocate(storage, slots);
        }
        // limit records the constructed slots, avoiding a second
        // array-allocation cookie and its allocator size-class rounding.
        std::string* values = nullptr;
        uint16_t limit = 0;
        std::vector<tail_block> tail;
        std::unique_ptr<block_map> remote;
        size_t tail_count = 0;
    };
    std::unique_ptr<dense_prefix> dense_;
    using sparse_map = std::unordered_map<uint64_t, std::string>;
    std::unique_ptr<sparse_map> sparse_;
    sparse_map& sparse() {
        if (!sparse_) {
            sparse_.reset(new sparse_map);
            sparse_->max_load_factor(2.0f);
        }
        return *sparse_;
    }

    void reclaim_sparse() noexcept {
        const auto buckets = sparse_->bucket_count();
        const auto count = sparse_->size();
        // Keep small tables for reuse. After a large contraction, retain
        // room for twice the live entries at the current maximum load.
        if (buckets < 256 || count > buckets / 4) return;
        if (!count) {
            sparse_.reset();
            return;
        }
        try {
            sparse_->rehash(count);
        } catch (const std::bad_alloc&) {
            // Optional compaction must not make deletion fail.
        }
    }

    void reclaim_remote() noexcept {
        auto& blocks = dense_->remote;
        if (blocks->empty()) { blocks.reset(); return; }
        const auto buckets = blocks->bucket_count();
        if (buckets < 256 || blocks->size() > buckets / 4) return;
        try { blocks->rehash(blocks->size() * 2); }
        catch (const std::bad_alloc&) {
            // Optional metadata contraction leaves all pages readable.
        }
    }

    static uint64_t tail_slot(uint64_t id) {
        return (id - prefix_limit - 1) / tail_limit;
    }
    tail_block* tail_for(uint64_t id) const {
        if (!dense_ || id <= prefix_limit) return nullptr;
        const auto block = tail_slot(id);
        if (block < dense_->tail.size() && dense_->tail[block].values)
            return &dense_->tail[block];
        if (!dense_->remote) return nullptr;
        const auto found = dense_->remote->find(block);
        return found == dense_->remote->end() ? nullptr : &found->second;
    }

    bool demote_dense_prefix() noexcept {
        // The dense prefix covers every small slot, so small_ is empty.
        decltype(small_) prepared_small;
        std::array<std::pair<uint64_t, std::string*>, small_limit> moved;
        size_t count = 0;
        try {
            for (size_t i = 0; i < dense_->limit; ++i) {
                if (!dense_->present[i]) continue;
                const auto id = i + 1;
                if (id <= small_limit) {
                    prepared_small.emplace(id, std::string());
                    moved[count++] = {id, const_cast<std::string*>(prepared_small.get(id))};
                } else {
                    const auto inserted = sparse().emplace(id, std::string());
                    if (!inserted.second) break;
                    moved[count++] = {id, &inserted.first->second};
                }
            }
        } catch (const std::bad_alloc&) {
            // Until preparation succeeds, the dense slots own the payloads.
        }
        if (count != dense_->count) {
            for (size_t i = 0; i < count; ++i)
                if (moved[i].first > small_limit) sparse_->erase(moved[i].first);
            if (sparse_ && sparse_->empty()) sparse_.reset();
            return false;
        }
        for (size_t i = 0; i < count; ++i)
            moved[i].second->swap(dense_->values[moved[i].first - 1]);
        small_.swap(prepared_small);
        dense_->present.reset();
        dense_->count = 0;
        return true;
    }

    bool release_tail_pages(uint64_t slot, tail_block& block) noexcept {
        std::array<std::pair<uint64_t, std::string*>, tail_release_limit> moved;
        size_t count = 0;
        const auto first = prefix_limit + slot * tail_limit + 1;
        try {
            auto& table = sparse();
            for (size_t i = 0; i < tail_limit; ++i) {
                if (!block.present[i]) continue;
                const auto inserted = table.emplace(first + i, std::string());
                if (!inserted.second) break;
                moved[count++] = {first + i, &inserted.first->second};
            }
        } catch (const std::bad_alloc&) {
            // The original block still owns every page.
        }
        if (count != block.present.count()) {
            for (size_t i = 0; i < count; ++i) sparse_->erase(moved[i].first);
            return false;
        }
        for (size_t i = 0; i < count; ++i)
            moved[i].second->swap((*block.values)[moved[i].first - first]);
        dense_->tail_count -= count;
        return true;
    }

    template<class Block>
    static bool insert_slot(Block& block, size_t index, const std::string& value) {
        if (block.present[index]) return false;
        // Copy construction keeps a fresh slot in the payload's size class.
        std::string(value).swap(block.values[index]);
        block.present[index] = true;
        ++block.count;
        return true;
    }

    template<class Block>
    static bool erase_slot(Block& block, size_t index) noexcept {
        if (!block.present[index]) return false;
        std::string().swap(block.values[index]);
        block.present[index] = false;
        --block.count;
        return true;
    }

    static bool insert_slot(tail_block& block, size_t index,
        const std::string& value) {
        if (block.present[index]) return false;
        std::string(value).swap((*block.values)[index]);
        block.present[index] = true;
        return true;
    }

    static bool erase_slot(tail_block& block, size_t index) noexcept {
        if (!block.present[index]) return false;
        std::string().swap((*block.values)[index]);
        block.present[index] = false;
        return true;
    }

    template<class Block>
    static const std::string* get_slot(const Block* block, size_t index) {
        return block && block->present[index] ? &block->values[index] : nullptr;
    }

    template<class Block, class Visitor>
    static bool visit_slots(const Block* block, uint64_t offset, Visitor& visit) {
        if (block)
            for (size_t i = 0; i < block->values.size(); ++i)
                if (block->present[i] && !visit(offset + i + 1, block->values[i]))
                    return false;
        return true;
    }

    struct legacy_small {
        std::unique_ptr<inline_pages> values;

        size_t size() const {
            return values ? values->count +
                (values->extension ? values->extension->count : 0) : 0;
        }
        const std::string* get(uint64_t id) const {
            if (!values) return nullptr;
            return id <= first_slots ? get_slot(values.get(), id - 1) :
                get_slot(values->extension.get(), id - first_slots - 1);
        }
        void emplace(uint64_t id, const std::string& value) {
            if (!values) values.reset(new inline_pages);
            if (id <= first_slots) {
                insert_slot(*values, id - 1, value);
            } else {
                auto& extension = values->extension;
                if (!extension) extension.reset(new fixed_pages<small_limit - first_slots>);
                insert_slot(*extension, id - first_slots - 1, value);
            }
        }
        void erase(uint64_t id) noexcept {
            if (!values) return;
            if (id <= first_slots) {
                erase_slot(*values, id - 1);
            } else {
                auto& extension = values->extension;
                if (extension && erase_slot(*extension, id - first_slots - 1) &&
                    !extension->count) extension.reset();
            }
            if (!values->count && !values->extension) values.reset();
        }
        void swap(legacy_small& other) noexcept { values.swap(other.values); }
        template<class Visitor>
        bool visit(Visitor& visit_page) const {
            return !values || (visit_slots(values.get(), 0, visit_page) &&
                visit_slots(values->extension.get(), first_slots, visit_page));
        }
    };

    // The concurrent backend transfers ownership of the heap-allocated
    // index itself. MDB swaps indexes in-place and keeps its small pages
    // separate so even short payloads retain their addresses on transfer.
    struct inline_small {
        struct tail {
            std::unique_ptr<fixed_pages<small_limit - first_slots>> extension;
            std::string second;
        };
        std::unique_ptr<tail> later;
        std::string first;
        bool first_present = false;
        bool second_present = false;

        inline_small() = default;
        inline_small(inline_small&& other) noexcept { swap(other); }
        inline_small& operator=(inline_small&& other) noexcept {
            inline_small replacement(std::move(other));
            swap(replacement);
            return *this;
        }
        size_t size() const {
            return first_present + second_present +
                (later && later->extension ? later->extension->count : 0);
        }
        const std::string* get(uint64_t id) const {
            if (id == 1) return first_present ? &first : nullptr;
            if (id == 2) return second_present ? &later->second : nullptr;
            return later ? get_slot(later->extension.get(), id - first_slots - 1) :
                nullptr;
        }
        void emplace(uint64_t id, const std::string& value) {
            if (id == 1) {
                if (first_present) return;
                std::string(value).swap(first);
                first_present = true;
            } else if (id == 2) {
                if (second_present) return;
                std::string bytes(value);
                if (!later) later.reset(new tail);
                bytes.swap(later->second);
                second_present = true;
            } else if (later && later->extension) {
                insert_slot(*later->extension, id - first_slots - 1, value);
            } else {
                std::unique_ptr<fixed_pages<small_limit - first_slots>> extension(
                    new fixed_pages<small_limit - first_slots>);
                insert_slot(*extension, id - first_slots - 1, value);
                if (!later) later.reset(new tail);
                later->extension.swap(extension);
            }
        }
        void erase(uint64_t id) noexcept {
            if (id == 1) {
                std::string().swap(first);
                first_present = false;
                return;
            }
            if (!later) return;
            if (id == 2) {
                std::string().swap(later->second);
                second_present = false;
            } else if (later->extension &&
                erase_slot(*later->extension, id - first_slots - 1) &&
                !later->extension->count) later->extension.reset();
            if (!second_present && !later->extension) later.reset();
        }
        void swap(inline_small& other) noexcept {
            later.swap(other.later);
            first.swap(other.first);
            std::swap(first_present, other.first_present);
            std::swap(second_present, other.second_present);
        }
        template<class Visitor>
        bool visit(Visitor& visit_page) const {
            if (first_present && !visit_page(1, first)) return false;
            if (second_present && !visit_page(2, later->second)) return false;
            return !later || visit_slots(later->extension.get(), first_slots, visit_page);
        }
    };
    typename std::conditional<CompactGrowth, legacy_small, inline_small>::type small_;

public:
    size_t size() const {
        return (sparse_ ? sparse_->size() : 0) + small_.size() +
            (dense_ ? dense_->count + dense_->tail_count : 0);
    }
    bool empty() const { return size() == 0; }

    bool has_small_pages(size_t count) const {
        return !dense_ && !sparse_ && small_.size() == count;
    }

    static size_t sparse_tail_offset(size_t consecutive) {
        if (consecutive <= prefix_limit) return 0;
        const auto partial = (consecutive - prefix_limit) % tail_limit;
        return partial && partial < tail_growth_min ? consecutive - partial : 0;
    }

    void reserve_dense_prefix(size_t consecutive) {
        if (consecutive >= 16)
            dense_.reset(new dense_prefix(consecutive < prefix_limit ?
                consecutive : prefix_limit));
    }

    // The caller has proved at least 16 pages with IDs 1 through count.
    // Build an empty index block by block, keeping short tails sparse.
    template<class Iterator, class Value>
    void build_consecutive(Iterator first, size_t count, Value value) {
        reserve_dense_prefix(count);
        auto& prefix = *dense_;
        for (size_t i = 0; i < prefix.limit; ++i, ++first)
            insert_slot(prefix, i, value(*first));

        const auto sparse_start = sparse_tail_offset(count);
        const auto dense_end = sparse_start ? sparse_start : count;
        const auto tail_count = dense_end - prefix.limit;
        prefix.tail.resize((tail_count + tail_limit - 1) / tail_limit);
        for (auto& block : prefix.tail) {
            block.values.reset(new std::array<std::string, tail_limit>);
            for (size_t i = 0; i < tail_limit && prefix.tail_count < tail_count;
                 ++i, ++first, ++prefix.tail_count)
                insert_slot(block, i, value(*first));
        }
        if (sparse_start) {
            auto& tail = sparse();
            for (auto id = sparse_start; id < count; ++id, ++first)
                tail.emplace(id + 1, value(*first));
        }
    }

    template<class Iterator, class PageId>
    void prepare_empty(Iterator first, Iterator last, PageId page_id) {
        size_t consecutive = 0;
        if (!dense_) {
            for (auto it = first; it != last; ++it) {
                if (page_id(*it) != consecutive + 1) break;
                ++consecutive;
            }
            reserve_dense_prefix(consecutive);
        }
        const auto count = static_cast<size_t>(last - first);
        const auto covered = dense_ && consecutive ? consecutive : 0;
        // Bulk producers already prove a dense first suffix before any
        // addresses are published; reserve it instead of staging hash nodes.
        if (CompactGrowth && dense_ && consecutive >= prefix_limit + tail_growth_min &&
            dense_->tail.empty()) {
            tail_block next;
            next.values.reset(new std::array<std::string, tail_limit>);
            dense_->tail.push_back(std::move(next));
        }
        if (count - covered > small_limit) sparse().reserve(count - covered);
    }

    const std::string* get(uint64_t id) const {
        if (dense_ && id) {
            const auto offset = id - 1;
            if (offset < dense_->limit)
                return dense_->present[offset] ? &dense_->values[offset] : nullptr;
            if (offset >= prefix_limit) {
                const auto block = (offset - prefix_limit) / tail_limit;
                if (block < dense_->tail.size() && dense_->tail[block].values) {
                    const auto& pages = dense_->tail[block];
                    const auto index = offset % tail_limit;
                    return pages.present[index] ? &(*pages.values)[index] : nullptr;
                }
                if (dense_->remote) {
                    const auto found = dense_->remote->find(block);
                    if (found != dense_->remote->end()) {
                        const auto& pages = found->second;
                        const auto index = offset % tail_limit;
                        return pages.present[index] ? &(*pages.values)[index] : nullptr;
                    }
                }
            }
        }
        if (id && id <= small_limit) return small_.get(id);
        if (!sparse_) return nullptr;
        const auto found = sparse_->find(id);
        return found == sparse_->end() ? nullptr : &found->second;
    }
    std::string* get(uint64_t id) {
        return const_cast<std::string*>(static_cast<const mdb_page_index_impl&>(*this).get(id));
    }

    void emplace(uint64_t id, const std::string& value, bool grow_tail = true) {
        if (dense_ && id && id <= dense_->limit) {
            insert_slot(*dense_, id - 1, value);
            return;
        }
        if (id && id <= small_limit) {
            small_.emplace(id, value);
            return;
        }
        const auto index = (id - 1) % tail_limit;
        if (auto* block = tail_for(id)) {
            if (insert_slot(*block, index, value)) ++dense_->tail_count;
            return;
        }
        if (grow_tail && dense_ && id > prefix_limit && index == 0 &&
            tail_slot(id) == dense_->tail.size() &&
            (dense_->tail.empty() ? !CompactGrowth &&
                dense_->limit == prefix_limit &&
                dense_->count >= prefix_limit * 3 / 4 :
                dense_->tail.back().present.count() >= tail_growth_min)) {
            // The first suffix block stays sparse until post-publication
            // density proves it useful. Later blocks can follow a dense one.
            // A few deletions in the previous block need not send the
            // entire growing suffix to the sparse index.
            // Never shadow a page already indexed in the sparse tail.
            bool available = true;
            if (sparse_) {
                for (size_t i = 0; i < tail_limit && available; ++i)
                    available = sparse_->find(id + i) == sparse_->end();
            }
            if (available) {
                tail_block next;
                next.values.reset(new std::array<std::string, tail_limit>);
                insert_slot(next, 0, value);
                dense_->tail.push_back(std::move(next));
                ++dense_->tail_count;
                return;
            }
        }
        sparse().emplace(id, value);
    }

    void erase(uint64_t id, bool reclaim = false) noexcept {
        if (dense_ && id && id <= dense_->limit) {
            if (erase_slot(*dense_, id - 1) && reclaim && !dense_->count) {
                // Published deletes can release the prefix independently
                // of live tail blocks and sparse pages.
                dense_->release_values();
                if (dense_->tail.empty() && !dense_->tail_count) dense_.reset();
            }
            return;
        }
        if (id && id <= small_limit) {
            small_.erase(id);
            return;
        }
        auto* block = tail_for(id);
        if (!block) {
            if (sparse_ && sparse_->erase(id) && sparse_->size() % tail_limit == 0)
                reclaim_sparse();
            return;
        }
        if (!erase_slot(*block, (id - 1) % tail_limit)) return;
        --dense_->tail_count;
        const auto slot = tail_slot(id);
        if (block->present.none() || (reclaim && block->present.count() == tail_release_limit &&
            release_tail_pages(slot, *block))) {
            if (slot < dense_->tail.size() && dense_->tail[slot].values) {
                dense_->tail[slot] = tail_block();
                while (!dense_->tail.empty() && !dense_->tail.back().values)
                    dense_->tail.pop_back();
                if (reclaim && dense_->tail.empty())
                    std::vector<tail_block>().swap(dense_->tail);
            } else {
                dense_->remote->erase(slot);
                if (reclaim || dense_->remote->empty()) reclaim_remote();
            }
        }
    }

    // Only call after publication has finished using stored page addresses.
    void reclaim_prefix() noexcept {
        if (!dense_ || !dense_->count || dense_->count > small_limit ||
            dense_->limit < small_limit * 4 || !demote_dense_prefix()) return;
        dense_->release_values();
        if (dense_->tail.empty() && !dense_->tail_count) dense_.reset();
    }

    void promote_prefix(size_t additions) noexcept {
        const size_t limit = dense_ ? dense_->limit : 0;
        const auto sparse_count = (sparse_ ? sparse_->size() : 0) + small_.size();
        // An unaligned bulk prefix can leave fewer than 16 final slots.
        // Finish that bounded suffix when present, even below a growth
        // milestone; otherwise its pages remain permanently sparse.
        const bool finishing_prefix = CompactGrowth && limit < prefix_limit &&
            prefix_limit - limit < 16 && sparse_count < 16 &&
            sparse_count >= prefix_limit - limit;
        if (limit == prefix_limit || (sparse_count < 16 && !finishing_prefix))
            return;
        const auto before = sparse_count > additions ? sparse_count - additions : 0;
        size_t threshold = 16;
        while (threshold <= before && threshold <= prefix_limit) threshold *= 2;
        if (!finishing_prefix &&
            (threshold > sparse_count || threshold > prefix_limit)) return;

        auto end = limit;
        while (end < prefix_limit && get(end + 1)) ++end;
        if (end - limit < 16 && (!CompactGrowth || end != prefix_limit)) {
            if (sparse_count < 32) return;
            // Early deletions can leave a hole before a mostly occupied
            // prefix. Consider the rest only at a growth milestone.
            const auto target = sparse_count < prefix_limit - limit ?
                limit + sparse_count : prefix_limit;
            auto occupied = end - limit;
            for (auto i = end + 1; i < target; ++i)
                occupied += get(i + 1) != nullptr;
            if (occupied < 16 || occupied * 4 < (target - limit) * 3) return;
            end = target;
        }
        try {
            std::unique_ptr<dense_prefix> next(new dense_prefix(end));
            if (dense_) {
                next->present = dense_->present;
                next->count = dense_->count;
                for (size_t i = 0; i < limit; ++i)
                    next->values[i].swap(dense_->values[i]);
            }
            for (auto i = limit; i < end; ++i) {
                auto* page = get(i + 1);
                if (!page) continue;
                next->values[i].swap(*page);
                next->present[i] = true;
                ++next->count;
                erase(i + 1);
            }
            if (dense_) {
                next->tail.swap(dense_->tail);
                next->remote.swap(dense_->remote);
                next->tail_count = dense_->tail_count;
            }
            dense_.swap(next);
            if (sparse_ && sparse_->empty()) sparse_.reset();
        } catch (const std::bad_alloc&) {
            // Promotion is optional. A failed allocation leaves the old
            // index usable and cannot turn a published write into an error.
        }
    }

    void promote_tail(uint64_t last_id) noexcept {
        if (last_id <= prefix_limit || last_id % tail_limit ||
            !sparse_ || sparse_->size() < tail_growth_min) return;
        const auto slot = tail_slot(last_id);
        const auto slots = dense_ ? dense_->tail.size() : 0;
        if (tail_for(last_id)) return;
        const bool remote = slot >= slots && slot - slots > tail_gap_limit;

        // A dense block can follow a short sparse gap. Probe only at the
        // block boundary. Distant dense ranges use a sparse block directory
        // instead of allocating a vector proportional to their page IDs.
        std::array<sparse_map::iterator, tail_limit> pages;
        const auto first = last_id - tail_limit + 1;
        size_t count = 0;
        for (size_t i = 0; i < tail_limit; ++i) {
            pages[i] = sparse_->find(first + i);
            count += pages[i] != sparse_->end();
        }
        if (count < tail_growth_min) return;
        tail_block* target = nullptr;
        try {
            std::unique_ptr<std::array<std::string, tail_limit>> replacement(
                new std::array<std::string, tail_limit>);
            std::unique_ptr<dense_prefix> created;
            if (!dense_) created.reset(new dense_prefix(0));
            auto& owner = created ? *created : *dense_;
            if (remote) {
                if (!owner.remote) owner.remote.reset(new block_map);
                const auto inserted = owner.remote->emplace(slot, tail_block());
                if (!inserted.second) return;
                target = &inserted.first->second;
            } else {
                if (slot >= owner.tail.size()) owner.tail.resize(slot + 1);
                target = &owner.tail[slot];
            }
            target->values = std::move(replacement);
            if (created) dense_ = std::move(created);
        } catch (const std::bad_alloc&) {
            return;
        }
        auto& block = *target;
        for (size_t i = 0; i < tail_limit; ++i) {
            if (pages[i] == sparse_->end()) continue;
            (*block.values)[i].swap(pages[i]->second);
            block.present[i] = true;
            sparse_->erase(pages[i]);
        }
        dense_->tail_count += count;
        if (CompactGrowth && sparse_->empty()) sparse_.reset();
        else reclaim_sparse();
    }

    void swap(mdb_page_index_impl& other) noexcept {
        dense_.swap(other.dense_);
        sparse_.swap(other.sparse_);
        small_.swap(other.small_);
    }

    template<class Visitor>
    bool visit(Visitor visit_page) const {
        if (dense_) {
            for (size_t i = 0; i < dense_->limit; ++i)
                if (dense_->present[i] && !visit_page(i + 1, dense_->values[i]))
                    return false;
            uint64_t offset = prefix_limit;
            for (const auto& block : dense_->tail) {
                if (block.values) {
                    for (size_t i = 0; i < tail_limit; ++i)
                        if (block.present[i] &&
                            !visit_page(offset + i + 1, (*block.values)[i])) return false;
                }
                offset += tail_limit;
            }
            if (dense_->remote)
                for (const auto& entry : *dense_->remote) {
                    const auto remote_offset = prefix_limit + entry.first * tail_limit;
                    const auto& block = entry.second;
                    for (size_t i = 0; i < tail_limit; ++i)
                        if (block.present[i] &&
                            !visit_page(remote_offset + i + 1, (*block.values)[i])) return false;
                }
        }
        if (!small_.visit(visit_page)) return false;
        if (sparse_)
            for (const auto& page : *sparse_)
                if (!visit_page(page.first, page.second)) return false;
        return true;
    }
};

// Keep the concurrent backend's measured growth and storage policy
// independent of default MDB.
using mdb_page_index = mdb_page_index_impl<true>;

} // namespace pkv
