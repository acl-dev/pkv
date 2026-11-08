#pragma once
#include "page_batch.h"

namespace pkv {
namespace page_measure {
inline bool valid_root(const db::batch_entry& root,
    const db::batch_entry* previous) {
    const auto action = root.action;
    return (!previous || previous->key < root.key) &&
        (action == db::update_action::keep ||
         action == db::update_action::write ||
         action == db::update_action::erase) &&
        !root.force_watch_invalidation &&
        (!root.maintenance_only || root.clear_pages) &&
        (!root.clear_pages || action == db::update_action::write) &&
        (action != db::update_action::write ||
         root.value.size() <= page_transaction::max_value_bytes);
}
inline bool valid_page(const db::batch_entry& root, const page_change& page,
    uint64_t previous) {
    return page.id && page.id > previous &&
        (page.action == db::update_action::write ||
         page.action == db::update_action::erase) &&
        page.value.size() <= page_transaction::max_value_bytes &&
        (page.action != db::update_action::erase || page.value.empty()) &&
        (page.action != db::update_action::write ||
         (root.action != db::update_action::erase &&
          (root.found || root.action == db::update_action::write)));
}
inline bool charge(page_batch_size& size, size_t key, size_t value) {
    if (size.records >= page_batch::max_records ||
        key > page_batch::max_key_bytes ||
        20 + key + value > page_batch::max_bytes - size.bytes) return false;
    size.bytes += 20 + key + value; ++size.records;
    return true;
}
// This charge bounds decoded payload and object storage, with string padding.
// It is not an allocator/RSS measurement. Legacy wire bounds imply it fits.
inline void storage(page_batch_size& size, size_t object, size_t key,
    size_t value) {
    size.expanded_bytes += object + key + value + 64;
    ++size.actions;
}
inline bool bounded_storage(page_batch_size& size, size_t object, size_t key,
    size_t value) {
    if (size.actions >= page_batch::max_expanded_records ||
        object + key + value + 64 >
        page_batch::max_expanded_bytes - size.expanded_bytes) return false;
    storage(size, object, key, value); return true;
}
} // namespace page_measure
} // namespace pkv
