#include "string_store.h"
#include "common/redis_string_util.h"
#include <algorithm>

namespace pkv {

string_store::string_store(page_transaction& transaction,
    const std::string& owner)
: transaction_(transaction), owner_(owner)
, entry_(transaction.roots().find(owner)) {}
string_store::~string_store() { if (dirty_) transaction_.abort(); }

bool string_store::fail(status value) {
    if (error_ == status::ok) error_ = value;
    if (value != status::not_paged) transaction_.abort();
    return false;
}
bool string_store::decoded(string_page_status value) {
    if (value == string_page_status::ok) return true;
    return fail(value == string_page_status::out_of_memory ? status::storage :
        value == string_page_status::unsupported ? status::unsupported :
            status::invalid);
}
bool string_store::open() {
    if (!entry_) return fail(status::invalid);
    if ((!entry_->found && entry_->action != db::update_action::write) ||
        entry_->action == db::update_action::erase) {
        root_ = string_root(); exists_ = false; return true;
    }
    if (entry_->value.compare(0, 3, "PSR") != 0)
        return fail(status::not_paged);
    if (!decoded(string_page_codec::decode_root(entry_->value, root_)))
        return false;
    exists_ = true;
    return true;
}
void string_store::initialize(int64_t expires_at_ms) {
    if (!entry_) { fail(status::invalid); return; }
    if (expires_at_ms < -1) { fail(status::invalid_argument); return; }
    if (!transaction_.reset_pages(owner_)) { fail(status::storage); return; }
    root_ = string_root(); root_.expires_at_ms = expires_at_ms;
    entry_->action = db::update_action::write;
    exists_ = dirty_ = true; error_ = status::ok;
}
bool string_store::read_page(uint64_t index, std::string& out) {
    if (index >= root_.pages) return fail(status::invalid);
    const auto result = transaction_.lookup(owner_, index + 1, out);
    if (result != db::lookup_status::found)
        return fail(result == db::lookup_status::error ? status::storage :
            status::invalid);
    return out.size() == string_page_codec::page_size(root_, index) ||
        fail(status::invalid);
}
bool string_store::write_page(uint64_t index, const std::string& value) {
    return transaction_.put(owner_, index + 1, value) || fail(status::storage);
}

bool string_store::assign(const std::string& value) {
    if (error_ != status::ok) return false;
    if (value.size() > string_page_codec::write_bytes)
        return fail(status::too_large);
    // Only the bounded inline member can alias our own metadata.
    std::string alias;
    const std::string* input = &value;
    if (input == &root_.inline_value) { alias = value; input = &alias; }
    const auto expiry = root_.expires_at_ms;
    initialize(expiry);
    if (error_ != status::ok) return false;
    root_.length = input->size();
    root_.pages = string_page_codec::pages_for(root_.length);
    if (!root_.pages) { root_.inline_value = *input; return true; }
    for (uint64_t i = 0; i < root_.pages; ++i) {
        const auto offset = static_cast<size_t>(i) *
            string_page_codec::page_bytes;
        const auto bytes = input->substr(offset,
            string_page_codec::page_size(root_, i));
        if (!write_page(i, bytes)) return false;
    }
    return true;
}

bool string_store::append(const std::string& value) {
    if (error_ != status::ok) return false;
    if (root_.length > string_page_codec::write_bytes ||
        value.size() > string_page_codec::write_bytes - root_.length)
        return fail(status::too_large);
    if (!exists_) initialize();
    if (error_ == status::ok) dirty_ = true;
    return error_ == status::ok && write(root_.length, value);
}

bool string_store::write(uint64_t offset, const std::string& value) {
    return write_limit(offset, value, string_page_codec::write_bytes);
}

bool string_store::write_field(uint64_t offset, const std::string& value) {
    return write_limit(offset, value, string_page_codec::max_bytes);
}

bool string_store::write_limit(uint64_t offset, const std::string& value,
    uint64_t limit) {
    if (error_ != status::ok) return false;
    if (value.empty()) return true;
    if (value.size() > limit || offset > limit - value.size())
        return fail(status::too_large);
    if (!exists_) initialize();
    if (error_ != status::ok) return false;
    const uint64_t end = offset + value.size();
    const auto length = std::max(root_.length, end);
    dirty_ = true;
    if (length <= string_page_codec::inline_bytes) {
        std::string bytes = root_.inline_value;
        bytes.resize(static_cast<size_t>(length), '\0');
        bytes.replace(static_cast<size_t>(offset), value.size(), value);
        root_.inline_value.swap(bytes); root_.length = length;
        return true;
    }
    auto first = offset / string_page_codec::page_bytes;
    auto last = (end - 1) / string_page_codec::page_bytes;
    const auto pages = string_page_codec::pages_for(length);
    if (!root_.pages) first = 0;
    if (length > root_.length) {
        first = std::min(first, root_.length / string_page_codec::page_bytes);
        last = pages - 1;
    }
    for (uint64_t i = first; i <= last; ++i) {
        std::string bytes;
        if (root_.pages && i < root_.pages) {
            if (!read_page(i, bytes)) return false;
        } else if (!root_.pages && i == 0) bytes = root_.inline_value;
        const auto begin = i * string_page_codec::page_bytes;
        const auto size = std::min<uint64_t>(string_page_codec::page_bytes,
            length - begin);
        bytes.resize(static_cast<size_t>(size), '\0');
        const auto left = std::max(begin, offset);
        const auto right = std::min(begin + size, end);
        if (left < right)
            bytes.replace(static_cast<size_t>(left - begin),
                static_cast<size_t>(right - left), value,
                static_cast<size_t>(left - offset),
                static_cast<size_t>(right - left));
        if (!write_page(i, bytes)) return false;
    }
    root_.length = length; root_.pages = pages; root_.inline_value.clear();
    return true;
}

bool string_store::read(std::string& out) { return range(0, -1, out); }
bool string_store::range(long long start, long long end, std::string& out) {
    out.clear();
    if (error_ != status::ok) return false;
    size_t offset, count;
    if (!exists_ || !string_slice(static_cast<size_t>(root_.length), start,
        end, offset, count)) return true;
    if (!root_.pages) {
        out.assign(root_.inline_value, offset, count); return true;
    }
    std::string result;
    result.reserve(count);
    while (count) {
        const auto index = offset / string_page_codec::page_bytes;
        const auto position = offset % string_page_codec::page_bytes;
        std::string bytes;
        if (!read_page(index, bytes)) return false;
        const auto size = std::min(count, bytes.size() - position);
        result.append(bytes, position, size);
        offset += size; count -= size;
    }
    out.swap(result);
    return true;
}

bool string_store::set_expiry(int64_t expires_at_ms) {
    if (error_ != status::ok) return false;
    if (expires_at_ms < -1) return fail(status::invalid_argument);
    if (exists_ && root_.expires_at_ms != expires_at_ms) {
        root_.expires_at_ms = expires_at_ms; dirty_ = true;
    }
    return true;
}
bool string_store::clear() {
    if (error_ != status::ok) return false;
    if (!exists_) return true;
    dirty_ = true;
    if (!transaction_.reset_pages(owner_)) return fail(status::storage);
    root_ = string_root(); exists_ = false;
    entry_->action = db::update_action::erase; entry_->clear_pages = false;
    return true;
}
bool string_store::finish() {
    if (error_ != status::ok) return false;
    if (!dirty_) return true;
    entry_->maintenance_only = false;
    if (!exists_) {
        entry_->action = db::update_action::erase; entry_->clear_pages = false;
    } else {
        if (!decoded(string_page_codec::encode_root(root_, entry_->value)))
            return false;
        entry_->action = db::update_action::write;
    }
    dirty_ = false;
    return true;
}

} // namespace pkv
