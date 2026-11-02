#include "list_store.h"
#include "common/redis_list_util.h"
#include <algorithm>
#include <climits>
#include <cstring>

namespace pkv {

const uint64_t list_store::max_member_bytes;

list_store::list_store(page_transaction& transaction, const std::string& owner)
: transaction_(transaction)
, owner_(owner)
, entry_(transaction.roots().find(owner))
{}

list_store::~list_store() {
    if (dirty_) transaction_.abort();
}

bool list_store::fail(status value) {
    if (error_ == status::ok) error_ = value;
    // A nonpaged format is a probe that an authorized importer may replace.
    if (value != status::not_paged) transaction_.abort();
    return false;
}

bool list_store::open() {
    if (!entry_) return fail(status::invalid);
    if ((!entry_->found && entry_->action != db::update_action::write) ||
        entry_->action == db::update_action::erase) {
        root_ = list_root();
        return true;
    }
    const auto result = list_page_codec::decode_root(entry_->value, root_);
    if (result == list_page_status::ok) {
        return root_.elements != 0 || fail(status::invalid);
    }
    if (entry_->value.compare(0, 3, "PLR") != 0) {
        return fail(status::not_paged);
    }
    return fail(result == list_page_status::unsupported_version ?
        status::unsupported : status::invalid);
}

void list_store::initialize(int64_t expires_at_ms) {
    if (!entry_) { fail(status::invalid); return; }
    if (!transaction_.reset_pages(owner_)) { fail(status::storage); return; }
    root_ = list_root();
    root_.expires_at_ms = expires_at_ms;
    entry_->clear_pages = true;
    entry_->action = db::update_action::write;
    error_ = status::ok;
    dirty_ = true;
}

bool list_store::read_page(uint64_t id, list_page& out) {
    std::string bytes;
    const auto result = transaction_.lookup(owner_, id, bytes);
    if (result != db::lookup_status::found) {
        return fail(result == db::lookup_status::error ? status::storage :
            status::invalid);
    }
    const auto decoded = list_page_codec::decode_page(bytes, out);
    if (decoded != list_page_status::ok) {
        return fail(decoded == list_page_status::out_of_memory ?
            status::storage : decoded == list_page_status::unsupported_version ?
                status::unsupported : status::invalid);
    }
    if (out.id != id || (root_.last_id && (id > root_.last_id ||
        out.previous > root_.last_id || out.next > root_.last_id)) ||
        (id == root_.head && out.previous != 0) ||
        (id == root_.tail && out.next != 0)) {
        return fail(status::invalid);
    }
    for (const auto& fragment : out.fragments) {
        if (fragment.total_bytes > max_member_bytes) {
            return fail(status::invalid);
        }
    }
    return true;
}

bool list_store::write_page(const list_page& page) {
    std::string bytes;
    const auto encoded = list_page_codec::encode_page(page, bytes);
    if (encoded != list_page_status::ok) {
        return fail(encoded == list_page_status::out_of_memory ?
            status::storage : status::invalid);
    }
    if (!transaction_.put(owner_, page.id, bytes)) {
        return fail(status::storage);
    }
    return true;
}

size_t list_store::used(const list_page& page) {
    size_t bytes = list_page_codec::page_header_bytes;
    for (const auto& fragment : page.fragments) {
        bytes += list_page_codec::fragment_header_bytes + fragment.bytes.size();
    }
    return bytes;
}

bool list_store::counter() {
    if (root_.last_id || !root_.elements) return true;
    // One-time promotion of roots created before the id counter was added.
    uint64_t id = root_.head, previous = 0, maximum = 0;
    std::unordered_set<uint64_t> visited;
    while (id) {
        list_page page;
        if (!visited.insert(id).second || !read_page(id, page) ||
            page.previous != previous) return fail(status::invalid);
        maximum = std::max(maximum, id);
        previous = id;
        id = page.next;
    }
    if (previous != root_.tail) return fail(status::invalid);
    root_.last_id = maximum;
    return true;
}

bool list_store::allocate(list_page& page) {
    if (root_.last_id == UINT64_MAX) return fail(status::too_large);
    page = list_page();
    page.id = ++root_.last_id;
    return true;
}

bool list_store::grow(list_page& page, bool left) {
    list_page next;
    if (!allocate(next)) return false;
    if (page.id) {
        if (left) {
            page.previous = next.id;
            next.next = page.id;
        } else {
            page.next = next.id;
            next.previous = page.id;
        }
        if (!write_page(page)) return false;
    } else {
        root_.head = root_.tail = next.id;
    }
    page = std::move(next);
    if (left) root_.head = page.id;
    else root_.tail = page.id;
    return true;
}

bool list_store::append(list_page& page, const std::string& value, bool left) {
    uint64_t offset = left ? value.size() : 0;
    bool first = true;
    while (first || (left ? offset != 0 : offset < value.size())) {
        first = false;
        const size_t available = list_page_codec::max_page_bytes - used(page);
        const size_t header = list_page_codec::fragment_header_bytes;
        if (!page.id || available < header + (value.empty() ? 0 : 1)) {
            if (!grow(page, left)) return false;
            first = true;
            continue;
        }
        const size_t remaining = left ? static_cast<size_t>(offset) :
            value.size() - static_cast<size_t>(offset);
        const auto count = std::min(remaining, available - header);
        list_fragment fragment;
        fragment.total_bytes = value.size();
        fragment.offset = left ? offset - count : offset;
        fragment.bytes.assign(value, static_cast<size_t>(fragment.offset),
            count);
        if (left) {
            page.fragments.insert(page.fragments.begin(), std::move(fragment));
            offset -= count;
        } else {
            page.fragments.push_back(std::move(fragment));
            offset += count;
        }
    }
    return true;
}

bool list_store::push(const std::vector<std::string>& values, bool left) {
    if (error_ != status::ok) return false;
    if (values.size() > static_cast<uint64_t>(LLONG_MAX) - root_.elements) {
        return fail(status::too_large);
    }
    for (const auto& value : values) {
        if (value.size() > max_member_bytes) return fail(status::too_large);
    }
    if (values.empty()) return true;
    dirty_ = true;
    if (!root_.elements) {
        if (!transaction_.reset_pages(owner_)) return fail(status::storage);
        entry_->action = db::update_action::write;
    }
    if (!counter()) return false;
    list_page page;
    if (root_.elements && !read_page(left ? root_.head : root_.tail, page)) {
        return false;
    }
    for (const auto& value : values) {
        if (!append(page, value, left)) return false;
        ++root_.elements;
    }
    if (!write_page(page)) return false;
    dirty_ = true;
    return true;
}

bool list_store::advance(list_page& page, bool left,
    std::unordered_set<uint64_t>& visited) {
    if (!page.fragments.empty()) return true;
    const auto previous = page.id;
    const auto next = left ? page.next : page.previous;
    if (!transaction_.erase(owner_, previous)) return fail(status::storage);
    if (!next) {
        page = list_page();
        root_.head = root_.tail = 0;
        return true;
    }
    if (!visited.insert(next).second || !read_page(next, page) ||
        (left ? page.previous : page.next) != previous) {
        return fail(status::invalid);
    }
    if (left) {
        page.previous = 0;
        root_.head = next;
    } else {
        page.next = 0;
        root_.tail = next;
    }
    return true;
}

bool list_store::take(list_page& page, bool left, std::string* out,
    std::unordered_set<uint64_t>& visited) {
    if (!advance(page, left, visited) || !page.id) return fail(status::invalid);
    const auto& first = left ? page.fragments.front() : page.fragments.back();
    const auto total = first.total_bytes;
    if (left ? first.offset != 0 :
        first.offset + first.bytes.size() != total) return
            fail(status::invalid);
    uint64_t offset = left ? 0 : total;
    if (out) out->assign(static_cast<size_t>(total), '\0');
    bool more;
    do {
        if (!advance(page, left, visited) || !page.id) {
            return fail(status::invalid);
        }
        const auto& fragment = left ? page.fragments.front() :
            page.fragments.back();
        if (fragment.total_bytes != total || (left ? fragment.offset != offset :
            fragment.offset + fragment.bytes.size() != offset)) {
            return fail(status::invalid);
        }
        if (out && !fragment.bytes.empty()) {
            std::memcpy(&(*out)[static_cast<size_t>(fragment.offset)],
                fragment.bytes.data(), fragment.bytes.size());
        }
        if (left) {
            offset += fragment.bytes.size();
            page.fragments.erase(page.fragments.begin());
        } else {
            offset = fragment.offset;
            page.fragments.pop_back();
        }
        more = left ? offset != total : offset != 0;
    } while (more);
    return true;
}

bool list_store::pop(uint64_t count, bool left,
    std::vector<std::string>* out) {
    if (out) out->clear();
    if (error_ != status::ok) return false;
    const auto take_count = std::min(count, root_.elements);
    if (!take_count) return true;
    dirty_ = true;
    list_page page;
    if (!read_page(left ? root_.head : root_.tail, page)) return false;
    std::unordered_set<uint64_t> visited{page.id};
    for (uint64_t i = 0; i < take_count; ++i) {
        std::string value;
        if (!take(page, left, out ? &value : nullptr, visited)) return false;
        if (out) out->push_back(std::move(value));
        --root_.elements;
    }
    if (!advance(page, left, visited)) return false;
    if ((root_.elements == 0) != (page.id == 0)) return fail(status::invalid);
    if (page.id && !write_page(page)) return false;
    dirty_ = true;
    return true;
}

bool list_store::set_expiry(int64_t expires_at_ms) {
    if (error_ != status::ok) return false;
    if (expires_at_ms < -1) return fail(status::invalid_argument);
    if (root_.elements && root_.expires_at_ms != expires_at_ms) {
        root_.expires_at_ms = expires_at_ms;
        dirty_ = true;
    }
    return true;
}

bool list_store::finish() {
    if (error_ != status::ok) return false;
    if (!dirty_) return true;
    entry_->maintenance_only = false;
    if (!root_.elements) {
        entry_->action = db::update_action::erase;
        entry_->clear_pages = false;
        dirty_ = false;
        return true;
    }
    const auto encoded = list_page_codec::encode_root(root_, entry_->value);
    if (encoded != list_page_status::ok) return fail(
        encoded == list_page_status::out_of_memory ? status::storage :
            status::invalid);
    entry_->action = db::update_action::write;
    dirty_ = false;
    return true;
}

} // namespace pkv
