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
    const auto result = transaction_.lookup_bound(entry_, index + 1, out, true);
    if (result != db::lookup_status::found)
        return fail(result == db::lookup_status::error ? status::storage :
            status::invalid);
    return out.size() == string_page_codec::page_size(root_, index) ||
        fail(status::invalid);
}
bool string_store::write_page(uint64_t index, std::string&& value) {
    return transaction_.put_bound(entry_, index + 1, std::move(value)) ||
        fail(status::storage);
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
    // A whole single-key write stages a fresh, consecutive page range.
    if (root_.pages > 1 && transaction_.roots().size() == 1)
        transaction_.reserve_sorted_pages(entry_, static_cast<size_t>(root_.pages));
    for (uint64_t i = 0; i < root_.pages; ++i) {
        const auto offset = static_cast<size_t>(i) *
            string_page_codec::page_bytes;
        if (!write_page(i, input->substr(offset,
                string_page_codec::page_size(root_, i)))) return false;
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
        const auto begin = i * string_page_codec::page_bytes;
        const auto size = std::min<uint64_t>(string_page_codec::page_bytes,
            length - begin);
        std::string bytes;
        if (i < root_.pages) {
            const auto old_size = string_page_codec::page_size(root_, i);
            std::string scratch;
            const bool growing = size > old_size;
            if (growing) bytes.reserve(static_cast<size_t>(size));
            const auto copy = [](void* raw, const std::string& page) {
                auto& target = *static_cast<std::string*>(raw);
                if (&target != &page) target = page;
            };
            // Reserve from empty before copying a growing page, so moving the
            // result does not retain capacity doubled from its previous size.
            if (!inspect_chunk(begin, old_size, growing ? scratch : bytes,
                    copy, &bytes)) return false;
        } else if (!root_.pages && i == 0) {
            bytes.reserve(static_cast<size_t>(size));
            bytes = root_.inline_value;
        }
        bytes.resize(static_cast<size_t>(size), '\0');
        const auto left = std::max(begin, offset);
        const auto right = std::min(begin + size, end);
        if (left < right)
            bytes.replace(static_cast<size_t>(left - begin),
                static_cast<size_t>(right - left), value,
                static_cast<size_t>(left - offset),
                static_cast<size_t>(right - left));
        if (!write_page(i, std::move(bytes))) return false;
    }
    root_.length = length; root_.pages = pages; root_.inline_value.clear();
    return true;
}

bool string_store::read(std::string& out) { return range(0, -1, out); }
bool string_store::inspect_chunk(uint64_t offset, size_t size,
    std::string& scratch, db::page_inspector visit, void* context) {
    if (error_ != status::ok) return false;
    if (!exists_ || !visit || !size || offset >= root_.length ||
        offset % string_page_codec::page_bytes)
        return fail(status::invalid);
    if (!root_.pages) {
        if (size != root_.inline_value.size()) return fail(status::invalid);
        visit(context, root_.inline_value);
        return true;
    }
    const auto index = offset / string_page_codec::page_bytes;
    if (size != string_page_codec::page_size(root_, index))
        return fail(status::invalid);
    struct inspection {
        size_t expected;
        db::page_inspector visit;
        void* context;
        bool valid;
    } work{size, visit, context, false};
    const auto validate = [](void* raw, const std::string& bytes) {
        auto& request = *static_cast<inspection*>(raw);
        if (bytes.size() != request.expected) return;
        request.valid = true;
        request.visit(request.context, bytes);
    };
    const auto result = transaction_.inspect_bound(entry_, index + 1,
        scratch, validate, &work);
    if (result == db::lookup_status::found && work.valid) return true;
    return fail(result == db::lookup_status::error ? status::storage :
        status::invalid);
}

bool string_store::range(long long start, long long end, std::string& out) {
    out.clear();
    if (error_ != status::ok) return false;
    size_t offset, count;
    if (!exists_ || !string_slice(static_cast<size_t>(root_.length), start,
        end, offset, count)) return true;
    if (!root_.pages) {
        out.assign(root_.inline_value, offset, count); return true;
    }
    const auto first_page = offset / string_page_codec::page_bytes;
    if (offset % string_page_codec::page_bytes == 0 &&
        count == string_page_codec::page_size(root_, first_page)) {
        if (read_page(first_page, out)) return true;
        out.clear();
        return false;
    }
    std::string result;
    result.reserve(count);
    std::string scratch;
    struct slice {
        std::string& result;
        size_t position;
        size_t count;
    };
    const auto append = [](void* context, const std::string& bytes) {
        auto& part = *static_cast<slice*>(context);
        part.result.append(bytes.data() + part.position, part.count);
    };
    while (count) {
        const auto index = offset / string_page_codec::page_bytes;
        const auto position = offset % string_page_codec::page_bytes;
        const auto page_size = string_page_codec::page_size(root_, index);
        const auto size = std::min(count, page_size - position);
        slice part{result, position, size};
        if (!inspect_chunk(offset - position, page_size, scratch, append, &part))
            return false;
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
