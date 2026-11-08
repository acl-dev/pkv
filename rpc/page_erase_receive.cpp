#include "page_batch.h"
#include "page_wire.h"
#include <algorithm>

namespace pkv {
bool page_receiver::storage(size_t key, size_t value, size_t count) {
    const auto bytes = key + value;
    if (bytes && count > (storage_limit_ - expanded_bytes_) / bytes)
        return false;
    expanded_bytes_ += count * bytes;
    return true;
}

bool page_receiver::append_erase_group(page_frame& frame) {
    auto& pages = batch_->pages_;
    if (protocol_ != 5 || frame.id || frame.value.size() < 16 ||
        frame.value.size() % 8 ||
        frame.value.size() / 8 > page_batch::max_erase_ids ||
        batch_->roots_.size() != expected_.roots) return false;
    const auto count = frame.value.size() / 8;
    if (count > expected_.pages - pages.size()) return false;
    if (!pages.empty() && frame.key < pages.back().owner) return false;
    const auto& roots = batch_->roots_;
    const auto root = std::lower_bound(roots.begin(), roots.end(), frame.key,
        [](const db::batch_entry& entry, const std::string& key) {
        return entry.key < key;
    });
    if (root == roots.end() || root->key != frame.key) return false;
    uint64_t previous = !pages.empty() && pages.back().owner == frame.key ?
        pages.back().id : 0;
    for (size_t i = 0; i < count; ++i) {
        const auto id = page_wire::get64(frame.value.data() + i * 8);
        if (!id || id <= previous) return false;
        previous = id;
    }
    if (!storage(frame.key.size(), 0, count)) return false;
    for (size_t i = 0; i < count; ++i)
        pages.emplace_back(frame.key,
            page_wire::get64(frame.value.data() + i * 8), std::string(),
            db::update_action::erase);
    return true;
}
} // namespace pkv
