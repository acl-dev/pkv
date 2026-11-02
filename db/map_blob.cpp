#include "map_store.h"
#include <algorithm>
#include <cstring>

namespace pkv {

map_store::blob_reader::blob_reader(map_store& store, const map_blob& blob)
: store_(store), blob_(blob), id_(blob.head) {}

bool map_store::blob_reader::next(map_fragment& page, bool& present) {
    present = false;
    if (store_.error_ != status::ok) return false;
    if (!blob_.head) {
        if (inline_done_ || !blob_.length) return true;
        map_fragment value;
        value.bytes = blob_.inline_value;
        page = std::move(value);
        inline_done_ = true; present = true;
        return true;
    }
    if (!id_) return offset_ == blob_.length || store_.fail(status::invalid);
    std::string bytes;
    map_fragment value;
    if (!store_.read(id_, bytes) || !store_.decoded(
        map_page_codec::decode_fragment(bytes, value))) return false;
    if (value.id != id_ || value.next > store_.last_id() ||
        value.offset != offset_ || offset_ > blob_.length ||
        value.bytes.size() > blob_.length - offset_)
        return store_.fail(status::invalid);
    const auto after = offset_ + value.bytes.size();
    if ((after == blob_.length) != (value.next == 0))
        return store_.fail(status::invalid);
    offset_ = after; id_ = value.next;
    page = std::move(value); present = true;
    return true;
}

bool map_store::visit_blob(const map_blob& blob,
    const fragment_visitor& callback) {
    if (!blob.head) return true;
    blob_reader reader(*this, blob);
    map_fragment page;
    bool present;
    while (reader.next(page, present)) {
        if (!present) return true;
        if (!callback(page)) return error_ == status::ok;
    }
    return false;
}

bool map_store::compare_blobs(const map_blob& left, const map_blob& right,
    int& order) {
    blob_reader first(*this, left), second(*this, right);
    map_fragment a, b;
    size_t offset_a = 0, offset_b = 0;
    bool have_a = true, have_b = true;
    order = 0;
    for (;;) {
        if (offset_a == a.bytes.size()) {
            if (!first.next(a, have_a)) return false;
            offset_a = 0;
        }
        if (offset_b == b.bytes.size()) {
            if (!second.next(b, have_b)) return false;
            offset_b = 0;
        }
        if (!have_a || !have_b) {
            order = have_a ? 1 : have_b ? -1 : 0;
            return true;
        }
        const auto count = std::min(a.bytes.size() - offset_a,
            b.bytes.size() - offset_b);
        order = std::memcmp(a.bytes.data() + offset_a,
            b.bytes.data() + offset_b, count);
        if (order) return true;
        offset_a += count; offset_b += count;
    }
}

bool map_store::compare(const map_blob& key, const std::string& input,
    int& order) {
    order = 0;
    if (!key.head) {
        order = key.inline_value.compare(input);
        return true;
    }
    return visit_blob(key, [&](const map_fragment& page) {
        const auto offset = static_cast<size_t>(page.offset);
        if (offset > input.size()) { order = 1; return false; }
        const auto length = std::min(page.bytes.size(), input.size() - offset);
        if (length) order = std::memcmp(page.bytes.data(), input.data() +
            offset, length);
        if (!order && length < page.bytes.size()) order = 1;
        if (!order && page.offset + length == key.length && key.length <
            input.size()) order = -1;
        if (!order && offset + length == input.size() && key.length >
            input.size()) order = 1;
        return order == 0;
    });
}

bool map_store::store_blob(const std::string& input, size_t limit,
    map_blob& out) {
    map_blob blob;
    blob.length = input.size();
    if (input.size() <= limit) {
        blob.inline_value = input;
        out = std::move(blob);
        return true;
    }
    const auto capacity = map_page_codec::max_page_bytes -
        map_page_codec::fragment_header_bytes;
    uint64_t id;
    if (!allocate(id)) return false;
    blob.head = id;
    size_t offset = 0;
    while (offset < input.size()) {
        map_fragment page;
        page.id = id; page.offset = offset;
        const auto length = std::min(capacity, input.size() - offset);
        page.bytes.assign(input, offset, length);
        offset += length;
        if (offset < input.size() && !allocate(page.next)) return false;
        std::string bytes;
        if (!decoded(map_page_codec::encode_fragment(page, bytes)))
            return false;
        if (!transaction_.put(owner_, id, bytes)) return fail(status::storage);
        id = page.next;
    }
    out = std::move(blob);
    return true;
}

bool map_store::read_blob(const map_blob& blob, std::string& out) {
    if (!blob.head) { out = blob.inline_value; return true; }
    std::string value;
    value.reserve(static_cast<size_t>(blob.length));
    if (!visit_blob(blob, [&](const map_fragment& page) {
        value += page.bytes;
        return true;
    })) return false;
    out.swap(value);
    return true;
}

bool map_store::erase_blob(const map_blob& blob) {
    return visit_blob(blob, [&](const map_fragment& page) {
        return transaction_.erase(owner_, page.id) || fail(status::storage);
    });
}

} // namespace pkv
