#include "page_transaction.h"

namespace pkv {

const size_t page_transaction::max_value_bytes;

bool page_transaction::valid_address(const std::string& owner, uint64_t id) {
    if (failed_ || id == 0 || roots_.find(owner) == nullptr) {
        failed_ = true;
        return false;
    }
    return true;
}

db::lookup_status page_transaction::lookup(const std::string& owner,
    uint64_t id, std::string& out) {
    if (!valid_address(owner, id)) {
        return db::lookup_status::error;
    }
    if (roots_.find(owner)->action == db::update_action::erase) {
        return db::lookup_status::missing;
    }
    const auto it = changes_.find(address(owner, id));
    if (it != changes_.end()) {
        if (it->second.action == db::update_action::erase) {
            return db::lookup_status::missing;
        }
        out = it->second.value;
        return db::lookup_status::found;
    }
    if (roots_.find(owner)->clear_pages) {
        return db::lookup_status::missing;
    }
    std::string bytes;
    const auto status = database_.dblookup_page(owner, id, bytes);
    if (status == db::lookup_status::error ||
        (status == db::lookup_status::found &&
            bytes.size() > max_value_bytes)) {
        failed_ = true;
        storage_error_ = true;
        return db::lookup_status::error;
    }
    if (status == db::lookup_status::found) {
        out.swap(bytes);
    }
    return status;
}

bool page_transaction::stage(const std::string& owner, uint64_t id,
    const std::string& bytes, db::update_action action) {
    if (!valid_address(owner, id) || bytes.size() > max_value_bytes) {
        failed_ = true;
        return false;
    }
    const address key(owner, id);
    auto it = changes_.find(key);
    if (it == changes_.end()) {
        changes_.emplace(key, page_change(owner, id, bytes, action));
    } else {
        it->second.value = bytes;
        it->second.action = action;
    }
    return true;
}

bool page_transaction::put(const std::string& owner, uint64_t id,
    const std::string& bytes) {
    return stage(owner, id, bytes, db::update_action::write);
}

bool page_transaction::erase(const std::string& owner, uint64_t id) {
    return stage(owner, id, "", db::update_action::erase);
}

bool page_transaction::reset_pages(const std::string& owner) {
    if (!valid_address(owner, 1)) return false;
    auto it = changes_.lower_bound(address(owner, 0));
    while (it != changes_.end() && it->first.first == owner) {
        it = changes_.erase(it);
    }
    roots_.find(owner)->clear_pages = true;
    return true;
}

bool page_transaction::replace_pages(const std::string& owner, uint64_t last) {
    if (!valid_address(owner, 1)) return false;
    auto it = changes_.lower_bound(address(owner, 0));
    uint64_t previous = 0;
    while (it != changes_.end() && it->first.first == owner &&
        it->first.second <= last) {
        if (previous == UINT64_MAX || it->first.second != previous + 1 ||
            it->second.action != db::update_action::write) {
            failed_ = true; return false;
        }
        previous = it->first.second;
        ++it;
    }
    if (previous != last) { failed_ = true; return false; }
    while (it != changes_.end() && it->first.first == owner)
        it = changes_.erase(it);
    roots_.find(owner)->clear_pages = true;
    return true;
}

bool page_transaction::valid_changes() const {
    if (failed_) {
        return false;
    }
    for (const auto& root : roots_) {
        if ((root.clear_pages && root.action != db::update_action::write) ||
            (root.action == db::update_action::write &&
                root.value.size() > max_value_bytes)) {
            return false;
        }
    }
    for (const auto& item : changes_) {
        const auto& change = item.second;
        const auto* root = roots_.find(change.owner);
        if (change.action == db::update_action::write &&
            (root->action == db::update_action::erase ||
                (!root->found && root->action != db::update_action::write))) {
            return false;
        }
    }
    return true;
}

std::vector<page_change> page_transaction::release_changes() {
    std::vector<page_change> out;
    out.reserve(changes_.size());
    for (auto& item : changes_) {
        out.push_back(std::move(item.second));
    }
    return out;
}

} // namespace pkv
