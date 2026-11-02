#include "string_store.h"

namespace pkv {

bool string_store::replace_chunks(uint64_t length, const chunk_source& source) {
    if (error_ != status::ok) return false;
    if (!entry_ || !source) return fail(status::invalid_argument);
    if (length > string_page_codec::max_bytes) return fail(status::too_large);
    if (length <= string_page_codec::inline_bytes) {
        std::string bytes;
        if (!source(0, static_cast<size_t>(length), bytes))
            return fail(status::storage);
        if (bytes.size() != length) return fail(status::invalid);
        return assign(bytes) && set_expiry(-1);
    }
    string_root replacement;
    replacement.length = length;
    replacement.pages = string_page_codec::pages_for(length);
    for (uint64_t i = 0; i < replacement.pages; ++i) {
        std::string bytes;
        const auto size = string_page_codec::page_size(replacement, i);
        if (!source(i * string_page_codec::page_bytes, size, bytes))
            return fail(status::storage);
        if (bytes.size() != size) return fail(status::invalid);
        if (!write_page(i, bytes)) return false;
    }
    // Seal after producing every page so old inputs remained readable. Drop
    // both the old family and any earlier staged edits beyond the new length.
    if (!transaction_.replace_pages(owner_, replacement.pages))
        return fail(status::storage);
    root_ = std::move(replacement);
    entry_->action = db::update_action::write;
    exists_ = dirty_ = true;
    return true;
}

} // namespace pkv
