#include "page_transaction.h"
#include "db_watcher.h"
#include <array>
#include <cassert>

namespace pkv {

const size_t page_transaction::max_value_bytes;

bool page_transaction::can_edit_list() const {
    return !failed_ && scope_ && scope_->cache_collections_ && roots_.size() == 1 &&
        roots_.begin()->found && !roots_.begin()->clear_pages && !has_changes() &&
        (!database_.watchers_ || !database_.watchers_->requires_page_payload());
}


namespace {
constexpr uint16_t wide_position_mask = 0x0fff;

uint16_t wide_id_tag(uint64_t id) {
    return static_cast<uint16_t>(((id >> 12) ^ (id >> 24) ^ (id >> 36)) << 12);
}
}

db::batch_entry* page_transaction::valid_address(const std::string& owner,
    uint64_t id) {
    auto* root = failed_ || id == 0 ? nullptr : roots_.find(owner);
    if (!root) {
        failed_ = true;
    }
    return root;
}

db::lookup_status page_transaction::lookup(const std::string& owner,
    uint64_t id, std::string& out) {
    return lookup_impl(owner, id, out, false);
}

db::lookup_status page_transaction::lookup_scratch(const std::string& owner,
    uint64_t id, std::string& out) {
    return lookup_impl(owner, id, out, true);
}

db::lookup_status page_transaction::lookup_impl(const std::string& owner,
    uint64_t id, std::string& out, bool reuse) {
    const auto* root = valid_address(owner, id);
    if (!root) {
        return db::lookup_status::error;
    }
    return lookup_page(root, owner, id, out, reuse);
}

const page_change* page_transaction::staged_page(const db::batch_entry* root,
    uint64_t id) const {
    if (sorted_owner_) {
        if (root != sorted_owner_) return nullptr;
        const auto it = sorted_page(id);
        return it != flat_.end() && it->id == id ? &*it : nullptr;
    }
    if (!flat_.empty()) {
        const auto* wide = wide_flat_index_.get();
        const auto position = wide ? wide[flat_slot(id, wide, wide_flat_index_size - 1)] &
                wide_position_mask
            : flat_index_[flat_slot(id, flat_index_, flat_index_size - 1)];
        return position ? &flat_[position - 1] : nullptr;
    }
    const auto it = changes_.find(address(ordinal(root), id));
    return it != changes_.end() ? &it->second : nullptr;
}

template<class Position>
size_t page_transaction::flat_slot(uint64_t id, const Position* index,
    size_t mask) const {
    size_t slot = (id ^ (id >> 6) ^ (id >> 12)) & mask;
    const auto tag = wide_id_tag(id);
    while (const auto entry = index[slot]) {
        // The wide index has four spare bits. A mismatch skips the page
        // record; a tag hit still has to match the full ID.
        if ((sizeof(Position) == 1 || (entry & ~wide_position_mask) == tag) &&
            flat_[(entry & wide_position_mask) - 1].id == id) break;
        slot = (slot + 1) & mask;
    }
    return slot;
}

std::string* page_transaction::staged_flat_write(uint64_t id) {
    if (failed_ || sorted_owner_ || flat_.empty() || !may_have_staged_id(id))
        return nullptr;
    // Both indexes address the same unsorted, single-owner vector.
    const auto* wide = wide_flat_index_.get();
    const auto position = wide ? wide[flat_slot(id, wide, wide_flat_index_size - 1)] &
            wide_position_mask
        : flat_index_[flat_slot(id, flat_index_, flat_index_size - 1)];
    if (!position) return nullptr;
    auto& page = flat_[position - 1];
    return page.action == db::update_action::write ? &page.value : nullptr;
}

void page_transaction::clear_flat_index() {
    wide_flat_index_.reset();
    std::fill_n(flat_index_, flat_index_size, 0);
}

void page_transaction::reserve_sorted_pages(const db::batch_entry* root,
    size_t count) {
    // Read-only roots do not prevent a single writer from using the vector.
    if (has_changes() || count > flat_.max_size()) return;
    flat_.reserve(count);
    sorted_owner_ = root;
}

std::string* page_transaction::append_sorted_page(db::batch_entry* root, uint64_t id) {
    if (root != sorted_owner_ || (!flat_.empty() && flat_.back().id >= id) ||
        !valid_bound(root, id)) return nullptr;
    flat_.emplace_back(root->key, id, std::string(), db::update_action::write);
    remember_staged_id(id);
    return &flat_.back().value;
}

std::vector<page_change>::iterator page_transaction::sorted_page(
    uint64_t id) const {
    if (flat_.empty() || flat_.back().id < id) return flat_.end();
    return std::lower_bound(flat_.begin(), flat_.end(), id,
        [](const page_change& page, uint64_t key) { return page.id < key; });
}

void page_transaction::spill_flat() {
    if (flat_.empty()) { sorted_owner_ = nullptr; return; }
    assert(changes_.empty());
    decltype(changes_) spilled;
    const auto owner = sorted_owner_ ? ordinal(sorted_owner_) : 0;
    // Allocate every map node before touching the live staging vector.
    // Until then the placeholder ID is the page's position in that vector.
    for (size_t i = 0; i < flat_.size(); ++i)
        spilled.emplace(std::piecewise_construct,
            std::forward_as_tuple(owner, flat_[i].id),
            std::forward_as_tuple("", i, "", db::update_action::keep));
    for (auto& item : spilled)
        item.second = std::move(flat_[item.second.id]);
    changes_.swap(spilled);
    flat_.clear();
    clear_flat_index();
    sorted_owner_ = nullptr;
}

db::lookup_status page_transaction::lookup_bound(db::batch_entry* root,
    uint64_t id, std::string& out, bool reuse) {
    if (!valid_bound(root, id)) return db::lookup_status::error;
    return lookup_page(root, root->key, id, out, reuse);
}

db::lookup_status page_transaction::lookup_page(const db::batch_entry* root,
    const std::string& owner, uint64_t id, std::string& out, bool reuse) {
    if (root->action == db::update_action::erase) {
        return db::lookup_status::missing;
    }
    if (first_matches(root, id)) {
        if (first_.action == db::update_action::erase)
            return db::lookup_status::missing;
        out = first_.value;
        return db::lookup_status::found;
    }
    if (const auto* staged = may_have_staged_id(id) ? staged_page(root, id) :
        nullptr) {
        if (staged->action == db::update_action::erase)
            return db::lookup_status::missing;
        out = staged->value;
        return db::lookup_status::found;
    }
    if (root->clear_pages) {
        return db::lookup_status::missing;
    }
    // Preserve the legacy output contract and aliased owner strings.
    const bool direct = reuse && &owner != &out;
    std::string temporary;
    auto& bytes = direct ? out : temporary;
    if (direct) bytes.clear();
    const auto status = database_.dblookup_page(owner, id, bytes);
    if (status == db::lookup_status::error ||
        (status == db::lookup_status::found &&
            bytes.size() > max_value_bytes)) {
        failed_ = true;
        storage_error_ = true;
        return db::lookup_status::error;
    }
    if (status == db::lookup_status::found && !direct) {
        out.swap(bytes);
    }
    return status;
}

template <class Value>
bool page_transaction::stage(const std::string& owner, uint64_t id,
    Value&& bytes, db::update_action action) {
    auto* root = valid_address(owner, id);
    if (!root || bytes.size() > max_value_bytes) {
        failed_ = true;
        return false;
    }
    return stage_page(root, id, std::forward<Value>(bytes), action);
}

template <class Value>
bool page_transaction::stage_bound(db::batch_entry* root, uint64_t id,
    Value&& bytes, db::update_action action) {
    if (!valid_bound(root, id) || bytes.size() > max_value_bytes) {
        failed_ = true;
        return false;
    }
    return stage_page(root, id, std::forward<Value>(bytes), action);
}

template <class Value>
bool page_transaction::stage_page(db::batch_entry* root, uint64_t id,
    Value&& bytes, db::update_action action) {
    const auto& owner = root->key;
    if (sorted_owner_ && root != sorted_owner_) spill_flat();
    if (sorted_owner_) {
        const auto it = sorted_page(id);
        if (it != flat_.end() && it->id == id) {
            it->value = std::forward<Value>(bytes); it->action = action;
        } else {
            // Copy the address before moving bytes: owner and bytes may alias.
            flat_.insert(it, page_change(owner, id,
                std::forward<Value>(bytes), action));
            remember_staged_id(id);
        }
        return true;
    }
    if (first_matches(root, id)) {
        first_.value = std::forward<Value>(bytes); first_.action = action;
        return true;
    }
    // Copy the address before moving bytes: owner and bytes may alias.
    if (!has_changes()) {
        std::string key;
        if (page_owner_buffer_ && owner.size() > 32 &&
            owner.size() <= page_owner_buffer_->capacity())
            key.swap(*page_owner_buffer_);
        key.assign(owner);
        page_change next(std::move(key), id, std::forward<Value>(bytes),
            action);
        first_ = std::move(next);
        return true;
    }
    if (roots_.size() == 1 && changes_.empty()) {
        auto* wide = wide_flat_index_.get();
        auto slot = wide ? flat_slot(id, wide, wide_flat_index_size - 1)
            : flat_slot(id, flat_index_, flat_index_size - 1);
        const auto position = wide ? wide[slot] & wide_position_mask : flat_index_[slot];
        if (position) {
            auto& page = flat_[position - 1];
            page.value = std::forward<Value>(bytes);
            page.action = action;
            return true;
        }
        if (flat_.size() < (wide_flat_enabled_ ? wide_flat_limit : flat_limit)) {
            if (flat_.empty()) flat_.reserve(initial_flat_limit + 1);
            if (flat_.size() == initial_flat_limit) flat_.reserve(flat_limit + 1);
            if (!wide && flat_.size() == flat_limit) {
                flat_.reserve(wide_reserve_limit + 1);
                std::unique_ptr<uint16_t[]> expanded(new uint16_t[wide_flat_index_size]());
                wide = expanded.get();
                for (size_t i = 0; i < flat_.size(); ++i) {
                    const auto page_id = flat_[i].id;
                    wide[flat_slot(page_id, wide, wide_flat_index_size - 1)] =
                        static_cast<uint16_t>(i + 1) | wide_id_tag(page_id);
                }
                slot = flat_slot(id, wide, wide_flat_index_size - 1);
                wide_flat_index_ = std::move(expanded);
            }
            if (flat_.size() == wide_reserve_limit)
                flat_.reserve(wide_flat_limit + 1);
            flat_.emplace_back(owner, id, std::forward<Value>(bytes), action);
            static_assert(wide_flat_limit <= wide_position_mask, "wide position fits");
            if (wide) wide[slot] = static_cast<uint16_t>(flat_.size()) | wide_id_tag(id);
            else flat_index_[slot] = static_cast<uint8_t>(flat_.size());
            remember_staged_id(id);
            return true;
        }
        spill_flat();
    }
    const address key(ordinal(root), id);
    auto it = may_have_staged_id(id) ? changes_.find(key) : changes_.end();
    if (it == changes_.end()) {
        changes_.emplace(key, page_change(owner, id,
            std::forward<Value>(bytes), action));
        remember_staged_id(id);
    } else {
        it->second.value = std::forward<Value>(bytes);
        it->second.action = action;
    }
    return true;
}

bool page_transaction::put(const std::string& owner, uint64_t id,
    const std::string& bytes) {
    return stage(owner, id, bytes, db::update_action::write);
}

bool page_transaction::put(const std::string& owner, uint64_t id,
    std::string&& bytes) {
    return stage(owner, id, std::move(bytes), db::update_action::write);
}

bool page_transaction::erase(const std::string& owner, uint64_t id) {
    return stage(owner, id, std::string(), db::update_action::erase);
}

bool page_transaction::put_bound(db::batch_entry* root, uint64_t id,
    const std::string& bytes) {
    return stage_bound(root, id, bytes, db::update_action::write);
}

bool page_transaction::put_bound(db::batch_entry* root, uint64_t id,
    std::string&& bytes) {
    return stage_bound(root, id, std::move(bytes), db::update_action::write);
}

bool page_transaction::put_tail_bound(db::batch_entry* root, uint64_t id,
    std::string&& bytes) {
    if (!valid_bound(root, id) || bytes.size() > max_value_bytes) {
        failed_ = true;
        return false;
    }
    const address key(ordinal(root), id);
    if (sorted_owner_ || (roots_.size() == 1 && changes_.empty()) || !has_changes() ||
        (first_.id == id && first_.owner == root->key) ||
        (!changes_.empty() && !(changes_.rbegin()->first < key))) {
        return stage_page(root, id, std::move(bytes), db::update_action::write);
    }
    changes_.emplace_hint(changes_.end(), key, page_change(root->key, id,
        std::move(bytes), db::update_action::write));
    remember_staged_id(id);
    return true;
}

bool page_transaction::erase_bound(db::batch_entry* root, uint64_t id) {
    return stage_bound(root, id, std::string(), db::update_action::erase);
}

void page_transaction::clear_first() {
    first_.id = 0; first_.owner.clear(); first_.value.clear();
}

void page_transaction::materialize_first() {
    if (!first_.id) return;
    // Copy on this cold path so an allocation failure retains the old view.
    const auto* root = roots_.find(first_.owner);
    assert(root);
    changes_.emplace(address(ordinal(root), first_.id), first_);
    remember_staged_id(first_.id);
    clear_first();
}

bool page_transaction::changes_owner(const std::string& owner) const {
    if (first_.id && first_.owner == owner) return true;
    const auto* root = roots_.find(owner);
    if (!root) return false;
    if (!flat_.empty()) return !sorted_owner_ || root == sorted_owner_;
    const auto index = ordinal(root);
    const auto it = changes_.lower_bound(address(index, 0));
    return it != changes_.end() && it->first.first == index;
}

bool page_transaction::reset_pages(const std::string& owner) {
    auto* root = valid_address(owner, 1);
    if (!root) return false;
    if (first_.id && first_.owner == owner) clear_first();
    if (!sorted_owner_ || root == sorted_owner_) {
        flat_.clear();
        clear_flat_index();
    }
    const auto index = ordinal(root);
    auto it = changes_.lower_bound(address(index, 0));
    while (it != changes_.end() && it->first.first == index) {
        it = changes_.erase(it);
    }
    root->clear_pages = true;
    return true;
}

bool page_transaction::replace_pages(const std::string& owner, uint64_t last) {
    auto* root = valid_address(owner, 1);
    if (!root) return false;
    spill_flat();
    materialize_first();
    const auto index = ordinal(root);
    auto it = changes_.lower_bound(address(index, 0));
    uint64_t previous = 0;
    while (it != changes_.end() && it->first.first == index &&
        it->first.second <= last) {
        if (previous == UINT64_MAX || it->first.second != previous + 1 ||
            it->second.action != db::update_action::write) {
            failed_ = true; return false;
        }
        previous = it->first.second;
        ++it;
    }
    if (previous != last) { failed_ = true; return false; }
    while (it != changes_.end() && it->first.first == index)
        it = changes_.erase(it);
    root->clear_pages = true;
    return true;
}

bool page_transaction::valid_roots(db::batch_view roots) {
    for (const auto& root : roots) {
        if ((root.clear_pages && root.action != db::update_action::write) ||
            (root.action == db::update_action::write &&
                root.value.size() > max_value_bytes)) {
            return false;
        }
    }
    return true;
}

bool page_transaction::valid_changes() const {
    if (failed_ || !valid_roots(roots_)) return false;
    if (roots_.size() == 1) {
        // Staging already bound each page to the sole root. If it can own
        // writes, all staged page actions are valid without another walk.
        const auto& root = *roots_.begin();
        if (root.action == db::update_action::write ||
            (root.action == db::update_action::keep && root.found))
            return true;
    }
    const auto valid = [&](const page_change& change) {
        const auto* root = roots_.find(change.owner);
        return change.action != db::update_action::write ||
            (root->action != db::update_action::erase &&
                (root->found || root->action == db::update_action::write));
    };
    if (first_.id && !valid(first_)) return false;
    for (const auto& page : flat_) if (!valid(page)) return false;
    for (const auto& item : changes_) if (!valid(item.second)) return false;
    return true;
}

std::vector<page_change> page_transaction::release_changes() {
    std::vector<page_change> out;
    release_changes(out);
    return out;
}

void page_transaction::release_changes(std::vector<page_change>& out) {
    assert(&out == &flat_ || out.empty());
    const bool first = first_.id != 0;
    if (!flat_.empty()) {
        if (&out != &flat_) {
            out.reserve(flat_.size() + static_cast<size_t>(first));
            for (auto& page : flat_) out.push_back(std::move(page));
        }
        if (first) out.push_back(std::move(first_));
        if (!sorted_owner_) {
            // Sort the positions first. Each page owns two strings; moving
            // records during comparisons needlessly moves their storage.
            std::array<uint64_t, wide_reserve_limit + 1> local_order;
            std::unique_ptr<uint64_t[]> large_order;
            assert(out.size() <= wide_flat_limit + 1);
            if (out.size() > local_order.size())
                large_order.reset(new uint64_t[out.size()]);
            auto* order = large_order ? large_order.get() : local_order.data();
            constexpr unsigned slot_bits = 12;
            constexpr uint64_t slot_mask = (uint64_t(1) << slot_bits) - 1;
            static_assert(wide_flat_limit <= slot_mask, "page position fits in sort key");
            size_t packed = 0;
            for (; packed < out.size(); ++packed) {
                const auto id = out[packed].id;
                if (id > (UINT64_MAX >> slot_bits)) break;
                order[packed] = (id << slot_bits) | packed;
            }
            if (packed == out.size()) {
                // Keep comparisons on the small array, without indirect
                // loads from the much larger staged page records.
                std::sort(order, order + packed);
                for (size_t i = 0; i < packed; ++i) order[i] &= slot_mask;
            } else {
                for (size_t i = 0; i < out.size(); ++i) order[i] = i;
                std::sort(order, order + out.size(),
                    [&](size_t a, size_t b) { return out[a].id < out[b].id; });
            }
            for (size_t i = 0; i < out.size(); ++i) {
                if (order[i] == i) continue;
                auto page = std::move(out[i]);
                size_t slot = i;
                while (order[slot] != i) {
                    const auto next = order[slot];
                    out[slot] = std::move(out[next]);
                    order[slot] = slot;
                    slot = next;
                }
                out[slot] = std::move(page);
                order[slot] = slot;
            }
        }
        return;
    }
    bool emitted = false;
    out.reserve(changes_.size() + static_cast<size_t>(first));
    for (auto& item : changes_) {
        if (first && !emitted) {
            const auto order = first_.owner.compare(
                roots_.begin()[item.first.first].key);
            if (order < 0 || (order == 0 && first_.id < item.first.second)) {
                out.push_back(std::move(first_)); emitted = true;
            }
        }
        out.push_back(std::move(item.second));
    }
    if (first && !emitted) out.push_back(std::move(first_));
}

} // namespace pkv
