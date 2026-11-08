#include "stdafx.h"
#include "string_bitop_source.h"

namespace pkv {
namespace dao {

bool string_bitop_source::stored() {
    error_ = string_read_status(native_->error());
    return native_->error() == string_store::status::ok;
}

bool string_bitop_source::open() {
    using status = dao_base::read_status;
    const auto& root = root_;
    if (root.action == db::update_action::erase ||
        (!root.found && root.action != db::update_action::write)) return true;
    if (root.value.compare(0, 3, "PSR") == 0) {
        native_.reset(new string_store(transaction_, root.key));
        if (!native_->open()) return stored();
        const auto& metadata = native_->metadata();
        if (metadata.expires_at_ms >= 0 && metadata.expires_at_ms <= now_) {
            expired_ = true; return true;
        }
        length_ = metadata.length;
        exists_ = true;
        return true;
    }
    const auto decoded = db_->value_codec().decode_stored(root.value, legacy_);
    if (decoded != codec_status::ok || !legacy_)
        return fail(decoded == codec_status::unsupported_version ?
            status::unsupported_format : decoded == codec_status::invalid_record
                ? status::invalid_record : status::storage_error);
    const auto& metadata = legacy_->header();
    if (metadata.expires_at_ms >= 0 && metadata.expires_at_ms <= now_) {
        expired_ = true; return true;
    }
    if (metadata.kind != value_kind::string) return fail(status::wrong_type);
    if (legacy_->paged_payload()) return fail(status::unsupported_format);
    size_t length;
    if (!legacy_->string_length(length)) return fail(status::invalid_record);
    if (length > string_page_codec::max_bytes)
        return fail(status::string_too_large);
    length_ = length;
    exists_ = true;
    return true;
}

bool string_bitop_source::read(uint64_t offset, size_t size, std::string& out) {
    out.clear();
    if (offset >= length_ || !size) return true;
    const auto count = static_cast<size_t>(std::min<uint64_t>(
        length_ - offset, size));
    const auto first = static_cast<long long>(offset);
    const auto last = static_cast<long long>(offset + count - 1);
    if (native_) {
        if (!native_->range(first, last, out)) return stored();
    } else if (!legacy_->string_range(first, last, out))
        return fail(dao_base::read_status::invalid_record);
    return out.size() == count || fail(dao_base::read_status::invalid_record);
}

bool string_bitop_source::fold(uint64_t offset, size_t size,
    bitmap_operation operation, char* output, char* mask, std::string& scratch) {
    if (native_ && offset <= length_ && size <= length_ - offset) {
        struct fold_input {
            bitmap_operation operation;
            char* output;
            char* mask;
        } work{operation, output, mask};
        const auto apply = [](void* raw, const std::string& bytes) {
            const auto& request = *static_cast<fold_input*>(raw);
            bitop_fold(request.operation, request.output, request.mask,
                bytes.data(), bytes.size());
        };
        return native_->inspect_chunk(offset, size, scratch, apply, &work) ||
            stored();
    }
    if (!read(offset, size, scratch)) return false;
    scratch.resize(size, '\0');
    bitop_fold(operation, output, mask, scratch.data(), size);
    return true;
}

} // namespace dao
} // namespace pkv
