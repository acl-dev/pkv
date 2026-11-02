#include "map_page.h"
#include <limits>
#include <new>
#include <utility>

namespace pkv {

const size_t map_page_codec::root_bytes;
const size_t map_page_codec::node_header_bytes;
const size_t map_page_codec::fragment_header_bytes;
const size_t map_page_codec::max_page_bytes;
const size_t map_page_codec::key_inline_bytes;
const size_t map_page_codec::value_inline_bytes;
const uint64_t map_page_codec::max_blob_bytes;
const uint64_t map_page_codec::max_payload_bytes;
const uint32_t map_page_codec::max_height;

namespace {

void put(std::string& out, uint64_t value, size_t count = 8) {
    for (size_t i = count; i; --i)
        out.push_back(static_cast<char>(value >> ((i - 1) * 8)));
}

uint64_t get(const std::string& bytes, size_t offset, size_t count = 8) {
    uint64_t value = 0;
    for (size_t i = 0; i < count; ++i)
        value = (value << 8) | static_cast<unsigned char>(bytes[offset + i]);
    return value;
}

map_page_status signature(const std::string& bytes, const char* magic,
    unsigned version = 1) {
    if (bytes.size() < 4 || bytes.compare(0, 3, magic, 3) != 0)
        return map_page_status::invalid;
    return static_cast<unsigned char>(bytes[3]) == version ?
        map_page_status::ok : map_page_status::unsupported;
}

bool valid(const map_root& root) {
    if ((root.kind != map_kind::hash && root.kind != map_kind::set) ||
        root.expires_at_ms < -1 || root.elements > static_cast<uint64_t>(
            std::numeric_limits<int64_t>::max())) return false;
    return root.elements ? root.tree && root.tree <= root.last_id :
        root.tree == 0;
}

bool valid(const map_blob& blob, size_t limit, uint64_t owner) {
    if (blob.length > map_page_codec::max_blob_bytes) return false;
    if (blob.head) return blob.head != owner && blob.length > limit &&
        blob.inline_value.empty();
    return blob.length <= limit && blob.length == blob.inline_value.size();
}

bool valid(const map_node& node) {
    return node.id && node.left != node.id && node.right != node.id &&
        (!node.left || node.left != node.right) &&
        (!node.key.head || node.key.head != node.value.head) && node.height &&
        node.height <= map_page_codec::max_height && node.elements &&
        node.elements <= static_cast<uint64_t>(
            std::numeric_limits<int64_t>::max()) &&
        valid(node.key, map_page_codec::key_inline_bytes, node.id) &&
        valid(node.value, map_page_codec::value_inline_bytes, node.id);
}

bool valid(const map_fragment& page) {
    return page.id && page.next != page.id && !page.bytes.empty() &&
        page.bytes.size() <= map_page_codec::max_page_bytes -
            map_page_codec::fragment_header_bytes &&
        page.offset <= map_page_codec::max_blob_bytes && page.bytes.size() <=
            map_page_codec::max_blob_bytes - page.offset;
}

} // namespace

map_page_status map_page_codec::encode_root(const map_root& root,
    std::string& out) {
    if (!valid(root)) return map_page_status::invalid;
    try {
        std::string bytes("PMR\2", 4);
        put(bytes, static_cast<unsigned char>(root.kind), 1);
        put(bytes, 0, 3);
        put(bytes, static_cast<uint64_t>(root.expires_at_ms));
        put(bytes, root.elements); put(bytes, root.tree);
        put(bytes, root.last_id);
        put(bytes, 0);
        out.swap(bytes);
        return map_page_status::ok;
    } catch (const std::bad_alloc&) { return map_page_status::out_of_memory; }
}

map_page_status map_page_codec::decode_root(const std::string& bytes,
    map_root& out) {
    const auto result = signature(bytes, "PMR", 2);
    if (result != map_page_status::ok) return result;
    if (bytes.size() != root_bytes || get(bytes, 5, 3) || get(bytes, 40))
        return map_page_status::invalid;
    map_root root;
    root.kind = static_cast<map_kind>(static_cast<unsigned char>(bytes[4]));
    const auto expiry = get(bytes, 8);
    if (expiry != std::numeric_limits<uint64_t>::max() && expiry >
        static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
        return map_page_status::invalid;
    root.expires_at_ms = expiry == std::numeric_limits<uint64_t>::max() ? -1 :
        static_cast<int64_t>(expiry);
    root.elements = get(bytes, 16);
    root.tree = get(bytes, 24); root.last_id = get(bytes, 32);
    if (!valid(root)) return map_page_status::invalid;
    out = root;
    return map_page_status::ok;
}

map_page_status map_page_codec::encode_node(const map_node& node,
    std::string& out) {
    if (!valid(node)) return map_page_status::invalid;
    try {
        std::string bytes("PMN\2", 4);
        put(bytes, node.bucket, 1); put(bytes, 0, 3);
        put(bytes, node.id); put(bytes, node.left);
        put(bytes, node.right); put(bytes, node.height, 4); put(bytes, 0, 4);
        put(bytes, node.elements);
        put(bytes, node.key.length); put(bytes, node.key.head);
        put(bytes, node.value.length); put(bytes, node.value.head);
        bytes += node.key.inline_value; bytes += node.value.inline_value;
        out.swap(bytes);
        return map_page_status::ok;
    } catch (const std::bad_alloc&) { return map_page_status::out_of_memory; }
}

map_page_status map_page_codec::decode_node(const std::string& bytes,
    map_node& out) {
    const auto result = signature(bytes, "PMN", 2);
    if (result != map_page_status::ok) return result;
    if (bytes.size() < node_header_bytes || bytes.size() > node_header_bytes +
        key_inline_bytes + value_inline_bytes || get(bytes, 5, 3) ||
        get(bytes, 36, 4)) return map_page_status::invalid;
    try {
        map_node node;
        node.bucket = static_cast<uint8_t>(get(bytes, 4, 1));
        node.id = get(bytes, 8); node.left = get(bytes, 16);
        node.right = get(bytes, 24);
        node.height = static_cast<uint32_t>(get(bytes, 32, 4));
        node.elements = get(bytes, 40);
        node.key.length = get(bytes, 48); node.key.head = get(bytes, 56);
        node.value.length = get(bytes, 64); node.value.head = get(bytes, 72);
        const auto keys = node.key.head ? 0 : node.key.length;
        const auto values = node.value.head ? 0 : node.value.length;
        if (keys > key_inline_bytes || values > value_inline_bytes ||
            bytes.size() != node_header_bytes + keys + values)
            return map_page_status::invalid;
        node.key.inline_value.assign(bytes, node_header_bytes,
            static_cast<size_t>(keys));
        node.value.inline_value.assign(bytes, node_header_bytes +
            static_cast<size_t>(keys), static_cast<size_t>(values));
        if (!valid(node)) return map_page_status::invalid;
        out = std::move(node);
        return map_page_status::ok;
    } catch (const std::bad_alloc&) { return map_page_status::out_of_memory; }
}

map_page_status map_page_codec::encode_fragment(const map_fragment& page,
    std::string& out) {
    if (!valid(page)) return map_page_status::invalid;
    try {
        std::string bytes("PMB\1", 4);
        put(bytes, 0, 4); put(bytes, page.id); put(bytes, page.next);
        put(bytes, page.offset); bytes += page.bytes;
        out.swap(bytes);
        return map_page_status::ok;
    } catch (const std::bad_alloc&) { return map_page_status::out_of_memory; }
}

map_page_status map_page_codec::decode_fragment(const std::string& bytes,
    map_fragment& out) {
    const auto result = signature(bytes, "PMB");
    if (result != map_page_status::ok) return result;
    if (bytes.size() <= fragment_header_bytes ||
        bytes.size() > max_page_bytes ||
        get(bytes, 4, 4)) return map_page_status::invalid;
    try {
        map_fragment page;
        page.id = get(bytes, 8); page.next = get(bytes, 16);
        page.offset = get(bytes, 24);
        page.bytes.assign(bytes, fragment_header_bytes, std::string::npos);
        if (!valid(page)) return map_page_status::invalid;
        out = std::move(page);
        return map_page_status::ok;
    } catch (const std::bad_alloc&) { return map_page_status::out_of_memory; }
}

} // namespace pkv
