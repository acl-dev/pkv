#include "map_page.h"
#include "common/redis_scan.h"
#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace pkv {

const size_t map_page_codec::root_bytes;
const size_t map_page_codec::node_header_bytes;
const size_t map_page_codec::fragment_header_bytes;
const size_t map_page_codec::max_page_bytes;
const size_t map_page_codec::key_inline_bytes;
const size_t map_page_codec::zset_key_inline_bytes;
const size_t map_page_codec::value_inline_bytes;
const uint64_t map_page_codec::max_blob_bytes;
const uint64_t map_page_codec::max_payload_bytes;
const uint32_t map_page_codec::max_height;

namespace {

constexpr size_t packed_node_header = 24;
constexpr size_t zset_reference_header = 11;
constexpr uint8_t zset_short_member_reference = 0xf6;
constexpr uint8_t zset_short_score_reference = 0xf7;
constexpr size_t zset_short_reference_header = 14;
constexpr size_t zset_short_reference_bytes = zset_short_reference_header + 8;
constexpr unsigned zset_short_reference_length_bits = 11;
constexpr uint64_t zset_short_reference_max_length = 1 << zset_short_reference_length_bits;
constexpr uint32_t zset_short_reference_max_height = 1U << (16 - zset_short_reference_length_bits);
constexpr uint8_t zset_reference_wide = 1;
constexpr uint8_t zset_reference_valued = 2;
constexpr size_t medium_node_header = 16;
constexpr size_t wide24_node_header = 19;
constexpr uint64_t wide24_id_max = 0xffffff;
constexpr size_t leaf_node_header = 11;
constexpr size_t leaf_inline_bytes = 11;
constexpr size_t short_leaf_header = 8;
constexpr size_t short_leaf_payload = 14;
constexpr size_t hashed_key_leaf_header = 7;
constexpr size_t hashed_key_leaf_max = 24;
constexpr uint8_t set_short_tag = 0xf4;
constexpr size_t set_short_header = 3;
constexpr size_t set_short_min = 16;
constexpr uint16_t set_short_id_mask = 0x3fff;
constexpr size_t set_reference_bytes = 20;
constexpr size_t zset_leaf_header = 6;
constexpr size_t zset_leaf_min = 15;
constexpr size_t zset_leaf_max = 25;
constexpr uint16_t zset_leaf_valued = 0x8000;
constexpr uint16_t zset_leaf_id_mask = zset_leaf_valued - 1;
constexpr size_t zset_short_header = 4;
constexpr size_t zset_short_payload = 26;
constexpr uint16_t zset_short_length = 0x4000;
constexpr uint16_t zset_short_id_mask = zset_short_length - 1;
constexpr uint16_t zset_short_signature = ('Z' << 8) | 'L';
constexpr uint8_t zset_wide_leaf_tag = 0xf5;
constexpr size_t zset_wide_leaf_header = 3;
constexpr size_t zset_wide_leaf_min = 28;
constexpr size_t short_branch_header = 8;
constexpr size_t compact_member_header = 11;
constexpr size_t compact_hash_leaf_header = 7;
constexpr size_t compact_hash_leaf_min = 24;
constexpr size_t compact_hash_leaf_max = 40;
constexpr size_t wide_leaf_header = 9;
constexpr uint8_t wide_leaf_hashed = 0x80;
constexpr size_t compact_member_min = 15;
constexpr size_t compact_member_max = 20;
constexpr uint64_t compact_member_id_mask = 0xfff;
constexpr uint64_t compact_member_count_max = 32;
constexpr uint32_t compact_member_height_max = 8;
constexpr size_t short_branch_payload = 14;
constexpr size_t valued_branch_key = 6;
constexpr size_t valued_branch_value = short_branch_payload - valued_branch_key;
constexpr uint32_t short_branch_max_height = 9;
constexpr uint64_t short_branch_max_elements = 32;
constexpr size_t small_node_header = 11;
constexpr size_t narrow_node_header = 10;
constexpr size_t narrow_payload = 12;
constexpr size_t compact_root_header = 12;
constexpr size_t compact_expiring_root = compact_root_header + 8;

void put(char* out, uint64_t value, size_t count = 8) {
#if defined(__GNUC__) && defined(__BYTE_ORDER__) && \
    __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    if (count == 8) {
        const auto encoded = __builtin_bswap64(value);
        std::memcpy(out, &encoded, sizeof(encoded));
        return;
    }
    if (count == 4) {
        const auto encoded = __builtin_bswap32(static_cast<uint32_t>(value));
        std::memcpy(out, &encoded, sizeof(encoded));
        return;
    }
    if (count == 2) {
        const auto encoded = __builtin_bswap16(static_cast<uint16_t>(value));
        std::memcpy(out, &encoded, sizeof(encoded));
        return;
    }
#endif
    for (size_t i = count; i; --i)
        *out++ = static_cast<char>(value >> ((i - 1) * 8));
}

uint64_t get(const char* bytes, size_t offset, size_t count = 8) {
#if defined(__GNUC__) && defined(__BYTE_ORDER__) && \
    __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    if (count == 8) {
        uint64_t value;
        std::memcpy(&value, bytes + offset, sizeof(value));
        return __builtin_bswap64(value);
    }
    if (count == 4) {
        uint32_t value;
        std::memcpy(&value, bytes + offset, sizeof(value));
        return __builtin_bswap32(value);
    }
    if (count == 2) {
        uint16_t value;
        std::memcpy(&value, bytes + offset, sizeof(value));
        return __builtin_bswap16(value);
    }
#elif defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    if (count == 8) {
        uint64_t value;
        std::memcpy(&value, bytes + offset, sizeof(value));
        return value;
    }
    if (count == 4) {
        uint32_t value;
        std::memcpy(&value, bytes + offset, sizeof(value));
        return value;
    }
#endif
    uint64_t value = 0;
    for (size_t i = 0; i < count; ++i)
        value = (value << 8) | static_cast<unsigned char>(bytes[offset + i]);
    return value;
}

uint64_t get(const std::string& bytes, size_t offset, size_t count = 8) {
    return get(bytes.data(), offset, count);
}

map_page_status signature(const std::string& bytes, const char* magic,
    unsigned version = 1) {
    if (bytes.size() < 4 || std::memcmp(bytes.data(), magic, 3) != 0)
        return map_page_status::invalid;
    return static_cast<unsigned char>(bytes[3]) == version ?
        map_page_status::ok : map_page_status::unsupported;
}

bool valid(const map_root& root) {
    if ((root.kind != map_kind::hash && root.kind != map_kind::set) ||
        (root.grouped && !root.bucketed) ||
        (root.bucketed && !root.grouped && root.kind != map_kind::hash) ||
        root.expires_at_ms < -1 || root.elements > static_cast<uint64_t>(
            std::numeric_limits<int64_t>::max())) return false;
    return root.elements ? root.tree && root.tree <= root.last_id :
        root.tree == 0;
}

bool valid_blob(uint64_t length, uint64_t head, size_t size, size_t limit,
    uint64_t owner) {
    if (length > map_page_codec::max_blob_bytes) return false;
    if (head) return head != owner && length > limit && !size;
    return length <= limit && length == size;
}

template<class Node>
#if defined(__GNUC__) || defined(__clang__)
__attribute__((always_inline))
#endif
inline bool valid_header(const Node& node) {
    return node.id && node.left != node.id && node.right != node.id &&
        (!node.left || node.left != node.right) &&
        (!node.key.head || node.key.head != node.value.head) && node.height &&
        node.height <= map_page_codec::max_height && node.elements &&
        node.elements <= static_cast<uint64_t>(
            std::numeric_limits<int64_t>::max());
}

bool valid(const map_node& node) {
    return valid_header(node) && valid_blob(node.key.length, node.key.head,
        node.key.inline_value.size(), node.key.head ?
            map_page_codec::key_inline_bytes : map_page_codec::zset_key_inline_bytes,
        node.id) && valid_blob(node.value.length, node.value.head,
        node.value.inline_value.size(), map_page_codec::value_inline_bytes,
        node.id);
}

template<class Node>
#if defined(__GNUC__) || defined(__clang__)
__attribute__((always_inline))
#endif
inline map_page_status read_zset_leaf(Node& node, uint16_t meta, const char* payload,
    size_t size) {
    const bool valued = meta & zset_leaf_valued;
    node.id = meta & zset_leaf_id_mask;
    if (!node.id) return map_page_status::invalid;
    node.value.length = valued ? 8 : 0;
    node.key.length = size - node.value.length;
    node.bucket = valued ? static_cast<uint8_t>(scan_bucket(payload,
        static_cast<size_t>(node.key.length))) : 0;
    node.bucket_derived = valued;
    node.left = node.right = node.key.head = node.value.head = 0;
    node.height = 1; node.elements = 1;
    return map_page_status::ok;
}

template<class Node>
map_page_status read_node_header(const std::string& bytes, Node& node,
    size_t& header) {
    using codec = map_page_codec;
    const auto size = bytes.size();
    const char* data = bytes.data();
    if (size < 4) return map_page_status::invalid;
    const auto prefix = get(data, 0, 4);
    constexpr uint32_t node_signature = ('P' << 16) | ('M' << 8) | 'N';
    if (prefix >> 8 != node_signature) {
        const auto tag = static_cast<uint8_t>(prefix >> 24);
        if (tag == zset_short_member_reference || tag == zset_short_score_reference) {
            if (size != zset_short_reference_bytes) return map_page_status::invalid;
            const bool score = tag == zset_short_score_reference;
            const auto length = (prefix & (zset_short_reference_max_length - 1)) + 1;
            node.bucket = static_cast<uint8_t>(prefix >> 16);
            if (length <= codec::zset_key_inline_bytes || (score && node.bucket))
                return map_page_status::invalid;
            header = zset_short_reference_header;
            node.bucket_derived = false;
            node.height = ((prefix & UINT16_MAX) >> zset_short_reference_length_bits) + 1;
            const auto fields = get(data, 4);
            node.id = fields >> 48;
            node.left = (fields >> 32) & UINT16_MAX;
            node.right = (fields >> 16) & UINT16_MAX;
            node.elements = fields & UINT16_MAX;
            node.key.head = get(data, 12, 2);
            node.key.length = length + (score ? 8 : 0);
            node.value.head = 0;
            node.value.length = score ? 0 : 8;
            return node.key.head && node.key.head != node.id && valid_header(node) ?
                map_page_status::ok : map_page_status::invalid;
        }
        if (tag == zset_wide_leaf_tag) {
            const auto meta = static_cast<uint16_t>(prefix >> 8);
            const auto payload = size - zset_wide_leaf_header;
            const auto limit = codec::zset_key_inline_bytes +
                (meta & zset_leaf_valued ? 8 : 0);
            if (payload < zset_wide_leaf_min || payload > limit)
                return map_page_status::invalid;
            header = zset_wide_leaf_header;
            return read_zset_leaf(node, meta, data + header, payload);
        }
        if (prefix >> 24 == set_short_tag) {
            const auto meta = static_cast<uint16_t>(prefix >> 8);
            header = set_short_header;
            node.id = meta & set_short_id_mask;
            node.key.length = set_short_min + (meta >> 14);
            if (!node.id || size != header + node.key.length)
                return map_page_status::invalid;
            node.bucket = static_cast<uint8_t>(scan_bucket(data + header,
                static_cast<size_t>(node.key.length)));
            node.bucket_derived = true;
            node.left = node.right = node.key.head = node.value.head = 0;
            node.value.length = 0;
            node.height = 1; node.elements = 1;
            return map_page_status::ok;
        }
        if (prefix >> 16 != zset_short_signature) return map_page_status::invalid;
        const auto meta = static_cast<uint16_t>(prefix);
        header = zset_short_header;
        if (size != header + zset_short_payload + bool(meta & zset_short_length))
            return map_page_status::invalid;
        return read_zset_leaf(node, meta & ~zset_short_length, data + header,
            size - header);
    }
    node.bucket_derived = false;
    const auto version = static_cast<uint8_t>(prefix);
    switch (version) {
    case 20: {
        if (size < zset_reference_header) return map_page_status::invalid;
        const auto flags = static_cast<uint8_t>(data[4]);
        if (flags > 2 || data[6]) return map_page_status::invalid;
        const auto width = 2U << flags;
        header = zset_reference_header + 5 * width;
        if (size != header + 8) return map_page_status::invalid;
        node.height = static_cast<uint8_t>(data[5]);
        node.bucket = 0;
        node.key.length = get(data, 7, 4);
        const auto* fields = data + zset_reference_header;
        const auto first = get(fields, 0);
        if (flags == 0) {
            node.id = first >> 48;
            node.left = (first >> 32) & UINT16_MAX;
            node.right = (first >> 16) & UINT16_MAX;
            node.elements = first & UINT16_MAX;
            node.key.head = get(fields, 8, 2);
        } else if (flags == 1) {
            const auto second = get(fields, 8);
            node.id = first >> 32;
            node.left = first & UINT32_MAX;
            node.right = second >> 32;
            node.elements = second & UINT32_MAX;
            node.key.head = get(fields, 16, 4);
        } else {
            node.id = first;
            node.left = get(fields, 8);
            node.right = get(fields, 16);
            node.elements = get(fields, 24);
            node.key.head = get(fields, 32);
        }
        node.value.length = node.value.head = 0;
        return valid_header(node) && node.key.head && node.key.head != node.id &&
            node.key.length > codec::zset_key_inline_bytes + 8 &&
            node.key.length <= codec::max_blob_bytes ?
            map_page_status::ok : map_page_status::invalid;
    }
    case 19: {
        if (size < zset_reference_header) return map_page_status::invalid;
        const auto flags = static_cast<uint8_t>(data[4]);
        if (flags & ~(zset_reference_wide | zset_reference_valued))
            return map_page_status::invalid;
        const bool wide = flags & zset_reference_wide;
        header = zset_reference_header + (wide ? 20 : 10);
        node.height = static_cast<uint8_t>(data[5]);
        node.bucket = static_cast<uint8_t>(data[6]);
        node.key.length = get(data, 7, 4);
        node.value.length = flags & zset_reference_valued ? 8 : 0;
        if (size != header + node.value.length ||
            (!node.value.length && node.bucket) ||
            node.key.length <= codec::key_inline_bytes ||
            node.key.length > (node.value.length ? codec::max_payload_bytes : codec::max_blob_bytes))
            return map_page_status::invalid;
        const auto links = get(data, zset_reference_header);
        if (wide) {
            const auto stats = get(data, zset_reference_header + 8);
            node.id = links >> 32;
            node.left = links & UINT32_MAX;
            node.right = stats >> 32;
            node.elements = stats & UINT32_MAX;
            node.key.head = get(data, zset_reference_header + 16, 4);
        } else {
            node.id = links >> 48;
            node.left = (links >> 32) & UINT16_MAX;
            node.right = (links >> 16) & UINT16_MAX;
            node.elements = links & UINT16_MAX;
            node.key.head = get(data, zset_reference_header + 8, 2);
        }
        node.value.head = 0;
        return node.key.head && node.key.head != node.id && valid_header(node) ?
            map_page_status::ok : map_page_status::invalid;
    }
    case 18: {
        header = set_reference_bytes;
        if (size != header) return map_page_status::invalid;
        node.bucket = static_cast<uint8_t>(data[4]);
        node.height = static_cast<uint8_t>(data[5]);
        node.id = get(data, 6, 2);
        node.left = get(data, 8, 2);
        node.right = get(data, 10, 2);
        node.elements = get(data, 12, 2);
        node.key.head = get(data, 14, 2);
        node.key.length = get(data, 16, 4);
        node.value.head = node.value.length = 0;
        return valid_header(node) && node.key.head && node.key.head != node.id &&
            node.key.length > codec::key_inline_bytes &&
            node.key.length <= codec::max_payload_bytes ?
            map_page_status::ok : map_page_status::invalid;
    }
    case 3:
    case 9: {
        const bool medium = version == 9;
        header = medium ? medium_node_header : packed_node_header;
        if (size < header) return map_page_status::invalid;
        node.bucket = static_cast<uint8_t>(data[4]);
        node.height = static_cast<uint8_t>(data[5]);
        node.key.length = static_cast<uint8_t>(data[6]);
        node.value.length = static_cast<uint8_t>(data[7]);
        if (medium) {
            const auto packed = get(data, 8);
            node.id = packed >> 48;
            node.left = (packed >> 32) & UINT16_MAX;
            node.right = (packed >> 16) & UINT16_MAX;
            node.elements = packed & UINT16_MAX;
        } else {
            const auto first = get(data, 8);
            const auto second = get(data, 16);
            node.id = first >> 32;
            node.left = first & UINT32_MAX;
            node.right = second >> 32;
            node.elements = second & UINT32_MAX;
        }
        node.key.head = node.value.head = 0;
        return node.key.length <= codec::zset_key_inline_bytes &&
            node.value.length <= codec::value_inline_bytes &&
            size == header + node.key.length + node.value.length &&
            valid_header(node) ? map_page_status::ok : map_page_status::invalid;
    }
    case 15: {
        header = wide24_node_header;
        if (size < header) return map_page_status::invalid;
        node.bucket = static_cast<uint8_t>(data[4]);
        node.height = static_cast<uint8_t>(data[5]);
        node.key.length = static_cast<uint8_t>(data[6]);
        node.value.length = static_cast<uint8_t>(data[7]);
        const auto links = get(data, 8);
        node.id = links >> 40;
        node.left = (links >> 16) & wide24_id_max;
        node.right = ((links & UINT16_MAX) << 8) | static_cast<uint8_t>(data[16]);
        node.elements = get(data, 17, 2);
        node.key.head = node.value.head = 0;
        return node.key.length <= codec::zset_key_inline_bytes &&
            node.value.length <= codec::value_inline_bytes &&
            size == header + node.key.length + node.value.length &&
            valid_header(node) ? map_page_status::ok : map_page_status::invalid;
    }
    case 14: {
        header = wide_leaf_header;
        if (size < header) return map_page_status::invalid;
        const auto meta = static_cast<uint8_t>(data[8]);
        const bool hashed = meta & wide_leaf_hashed;
        const size_t keys = meta & ~wide_leaf_hashed;
        const auto payload = size - header;
        if (keys > codec::zset_key_inline_bytes || keys > payload ||
            payload - keys > codec::value_inline_bytes ||
            (hashed ? payload == keys : keys <= short_leaf_payload || payload != keys))
            return map_page_status::invalid;
        node.id = get(data, 4, 4);
        node.bucket = hashed ? static_cast<uint8_t>(
            scan_bucket(data + header, keys)) : 0;
        node.bucket_derived = hashed;
        node.left = node.right = node.key.head = node.value.head = 0;
        node.height = 1; node.elements = 1;
        node.key.length = keys; node.value.length = payload - keys;
        return node.id > UINT16_MAX ? map_page_status::ok : map_page_status::invalid;
    }
    case 16: {
        header = hashed_key_leaf_header;
        if (size <= header + short_leaf_payload ||
            size > header + hashed_key_leaf_max) return map_page_status::invalid;
        node.id = get(data, 4, 2);
        node.key.length = size - header;
        node.bucket = static_cast<uint8_t>(data[6]);
        node.left = node.right = node.key.head = node.value.head = 0;
        node.height = 1; node.elements = 1; node.value.length = 0;
        return node.id ? map_page_status::ok : map_page_status::invalid;
    }
    case 17: {
        header = zset_leaf_header;
        if (size < header + zset_leaf_min || size > header + zset_leaf_max)
            return map_page_status::invalid;
        return read_zset_leaf(node, static_cast<uint16_t>(get(data, 4, 2)),
            data + header, size - header);
    }
    case 5: {
        header = small_node_header;
        if (size < header) return map_page_status::invalid;
        node.bucket = static_cast<uint8_t>(data[4]);
        const auto lengths = static_cast<uint8_t>(data[5]);
        node.key.length = lengths >> 4; node.value.length = lengths & 15;
        node.height = static_cast<uint8_t>(data[6]);
        node.id = static_cast<uint8_t>(data[7]);
        node.left = static_cast<uint8_t>(data[8]);
        node.right = static_cast<uint8_t>(data[9]);
        node.elements = static_cast<uint8_t>(data[10]);
        node.key.head = node.value.head = 0;
        const auto payload = node.key.length + node.value.length;
        return payload <= leaf_inline_bytes &&
            size == header + payload && valid_header(node) ?
            map_page_status::ok : map_page_status::invalid;
    }
    case 4: {
        header = leaf_node_header;
        if (size < header) return map_page_status::invalid;
        node.bucket = static_cast<uint8_t>(data[4]);
        node.key.length = static_cast<uint8_t>(data[5]);
        node.value.length = static_cast<uint8_t>(data[6]);
        node.id = get(data, 7, 4);
        node.left = node.right = node.key.head = node.value.head = 0;
        node.height = 1; node.elements = 1;
        const auto payload = node.key.length + node.value.length;
        return node.id && payload <= leaf_inline_bytes &&
            size == header + payload ?
            map_page_status::ok : map_page_status::invalid;
    }
    case 6: {
        header = narrow_node_header;
        if (size != header + narrow_payload)
            return map_page_status::invalid;
        node.bucket = static_cast<uint8_t>(data[4]);
        const auto metadata = static_cast<uint8_t>(data[5]);
        node.key.length = metadata >> 4;
        if (node.key.length > narrow_payload) return map_page_status::invalid;
        node.value.length = narrow_payload - node.key.length;
        node.height = metadata & 15;
        node.id = static_cast<uint8_t>(data[6]);
        node.left = static_cast<uint8_t>(data[7]);
        node.right = static_cast<uint8_t>(data[8]);
        node.elements = static_cast<uint8_t>(data[9]);
        node.key.head = node.value.head = 0;
        return valid_header(node) ? map_page_status::ok : map_page_status::invalid;
    }
    case 7: {
        header = short_leaf_header;
        if (size < header || size > header + short_leaf_payload)
            return map_page_status::invalid;
        node.bucket = static_cast<uint8_t>(data[4]);
        const auto lengths = static_cast<uint8_t>(data[5]);
        node.key.length = lengths >> 4; node.value.length = lengths & 15;
        if (size != header + node.key.length + node.value.length)
            return map_page_status::invalid;
        node.id = get(data, 6, 2);
        node.left = node.right = node.key.head = node.value.head = 0;
        node.height = 1; node.elements = 1;
        return node.id ? map_page_status::ok : map_page_status::invalid;
    }
    case 8:
    case 13: {
        header = short_branch_header;
        if (size != header + short_branch_payload)
            return map_page_status::invalid;
        const auto metadata = static_cast<uint8_t>(data[4]);
        const bool valued = version == 13;
        node.key.length = valued ? valued_branch_key : short_branch_payload;
        node.value.length = valued ? valued_branch_value : 0;
        node.bucket = valued ? static_cast<uint8_t>(
            scan_bucket(data + header, valued_branch_key)) : 0;
        node.bucket_derived = valued;
        node.height = (metadata >> 5) + 2;
        node.elements = (metadata & 31) + 1;
        node.id = static_cast<uint8_t>(data[5]);
        node.left = static_cast<uint8_t>(data[6]);
        node.right = static_cast<uint8_t>(data[7]);
        node.key.head = node.value.head = 0;
        return (node.left || node.right) && node.elements >= 2 && valid_header(node) ?
            map_page_status::ok : map_page_status::invalid;
    }
    case 11: {
        header = compact_member_header;
        if (size < header + compact_member_min ||
            size > header + compact_member_max)
            return map_page_status::invalid;
        const auto links = get(data, 6, 5);
        if (links >> 36) return map_page_status::invalid;
        const auto metadata = static_cast<uint8_t>(data[5]);
        node.bucket = static_cast<uint8_t>(data[4]);
        node.height = (metadata >> 5) + 1;
        node.elements = (metadata & 31) + 1;
        node.id = links >> 24;
        node.left = (links >> 12) & compact_member_id_mask;
        node.right = links & compact_member_id_mask;
        node.key.length = size - header;
        node.value.length = node.key.head = node.value.head = 0;
        return valid_header(node) ? map_page_status::ok : map_page_status::invalid;
    }
    case 12: {
        header = compact_hash_leaf_header;
        if (size < header + compact_hash_leaf_min ||
            size > header + compact_hash_leaf_max)
            return map_page_status::invalid;
        const auto payload = size - header;
        node.key.length = static_cast<uint8_t>(data[6]);
        if (node.key.length >= payload) return map_page_status::invalid;
        node.value.length = payload - node.key.length;
        node.bucket = static_cast<uint8_t>(scan_bucket(data + header,
            static_cast<size_t>(node.key.length)));
        node.bucket_derived = true;
        node.id = get(data, 4, 2);
        node.left = node.right = node.key.head = node.value.head = 0;
        node.height = 1; node.elements = 1;
        return node.id ? map_page_status::ok : map_page_status::invalid;
    }
    case 2: {
        header = codec::node_header_bytes;
        if (size < codec::node_header_bytes ||
            size > codec::node_header_bytes + codec::zset_key_inline_bytes +
                codec::value_inline_bytes || get(data, 5, 3) ||
            get(data, 36, 4)) return map_page_status::invalid;
        node.bucket = static_cast<uint8_t>(get(data, 4, 1));
        node.id = get(data, 8); node.left = get(data, 16);
        node.right = get(data, 24);
        node.height = static_cast<uint32_t>(get(data, 32, 4));
        node.elements = get(data, 40);
        node.key.length = get(data, 48); node.key.head = get(data, 56);
        node.value.length = get(data, 64); node.value.head = get(data, 72);
        const auto keys = node.key.head ? 0 : node.key.length;
        const auto values = node.value.head ? 0 : node.value.length;
        if (keys > codec::zset_key_inline_bytes || values > codec::value_inline_bytes ||
            size != codec::node_header_bytes + keys + values ||
            !valid_header(node) || !valid_blob(node.key.length, node.key.head,
                static_cast<size_t>(keys), node.key.head ? codec::key_inline_bytes :
                    codec::zset_key_inline_bytes, node.id) ||
            !valid_blob(node.value.length, node.value.head,
                static_cast<size_t>(values), codec::value_inline_bytes, node.id))
            return map_page_status::invalid;
        return map_page_status::ok;
    }
    default:
        return map_page_status::unsupported;
    }
}

bool shares_zset_reference(const std::string& bytes) {
    return static_cast<uint8_t>(bytes[0]) == zset_short_score_reference ||
        (bytes[0] == 'P' && bytes[3] == '\24');
}

bool fits_short_zset_reference(const map_node& node, uint64_t fields, uint64_t length) {
    return fields <= UINT16_MAX && length > map_page_codec::zset_key_inline_bytes &&
        length <= zset_short_reference_max_length &&
        node.height <= zset_short_reference_max_height &&
        std::string().capacity() >= zset_short_reference_bytes;
}

map_page_status encode_short_zset_reference(const map_node& node, bool score,
    uint64_t member_length, std::string& out) {
    char data[zset_short_reference_bytes];
    data[0] = static_cast<char>(score ? zset_short_score_reference : zset_short_member_reference);
    data[1] = static_cast<char>(node.bucket);
    put(data + 2, ((node.height - 1) << zset_short_reference_length_bits) |
        (member_length - 1), 2);
    put(data + 4, (node.id << 48) | (node.left << 32) | (node.right << 16) | node.elements);
    put(data + 12, node.key.head, 2);
    const auto& word = score ? node.key.inline_value : node.value.inline_value;
    std::memcpy(data + zset_short_reference_header, word.data(), 8);
    try {
        out.assign(data, sizeof(data));
        return map_page_status::ok;
    } catch (const std::bad_alloc&) { return map_page_status::out_of_memory; }
}

bool valid_fragment(uint64_t id, uint64_t next, uint64_t offset, size_t size) {
    return id && next != id && size &&
        size <= map_page_codec::max_page_bytes -
            map_page_codec::fragment_header_bytes &&
        offset <= map_page_codec::max_blob_bytes &&
        size <= map_page_codec::max_blob_bytes - offset;
}

} // namespace

map_page_status map_page_codec::encode_root(const map_root& root,
    std::string& out) {
    if (!valid(root)) return map_page_status::invalid;
    try {
        if ((!root.bucketed || root.grouped) && root.elements <= UINT16_MAX &&
            root.last_id <= UINT16_MAX) {
            char data[compact_expiring_root];
            std::memcpy(data, "PMR", 3);
            data[3] = root.grouped ? 5 : 4;
            data[4] = static_cast<char>(root.kind);
            const bool expiring = root.expires_at_ms >= 0;
            data[5] = expiring;
            put(data + 6, root.elements, 2);
            put(data + 8, root.tree, 2);
            put(data + 10, root.last_id, 2);
            if (expiring) put(data + compact_root_header, root.expires_at_ms);
            out.assign(data, expiring ? sizeof(data) : compact_root_header);
            return map_page_status::ok;
        }
        char data[root_bytes] = {};
        std::memcpy(data, "PMR\2", 4);
        if (root.bucketed) { data[3] = 3; data[5] = root.grouped ? 2 : 1; }
        data[4] = static_cast<char>(root.kind);
        put(data + 8, static_cast<uint64_t>(root.expires_at_ms));
        put(data + 16, root.elements); put(data + 24, root.tree);
        put(data + 32, root.last_id);
        out.assign(data, root_bytes);
        return map_page_status::ok;
    } catch (const std::bad_alloc&) { return map_page_status::out_of_memory; }
}

map_page_status map_page_codec::decode_root(const std::string& bytes,
    map_root& out) {
    if (bytes.size() >= 4 && (bytes[3] == 4 || bytes[3] == 5) &&
        std::memcmp(bytes.data(), "PMR", 3) == 0) {
        if (bytes.size() < compact_root_header || (bytes[5] != 0 && bytes[5] != 1))
            return map_page_status::invalid;
        const bool expiring = bytes[5] == 1;
        if (bytes.size() != (expiring ? compact_expiring_root :
            compact_root_header)) return map_page_status::invalid;
        map_root root;
        root.bucketed = root.grouped = bytes[3] == 5;
        root.kind = static_cast<map_kind>(static_cast<unsigned char>(bytes[4]));
        if (expiring) {
            const auto deadline = get(bytes, compact_root_header);
            if (deadline > static_cast<uint64_t>(INT64_MAX))
                return map_page_status::invalid;
            root.expires_at_ms = static_cast<int64_t>(deadline);
        }
        root.elements = get(bytes, 6, 2);
        root.tree = get(bytes, 8, 2);
        root.last_id = get(bytes, 10, 2);
        if (!valid(root)) return map_page_status::invalid;
        out = root;
        return map_page_status::ok;
    }
    const bool bucketed = bytes.size() >= 4 &&
        std::memcmp(bytes.data(), "PMR", 3) == 0 && bytes[3] == 3;
    if (bucketed && (bytes.size() < 6 || (bytes[5] != 1 && bytes[5] != 2)))
        return map_page_status::unsupported;
    const auto result = signature(bytes, "PMR", bucketed ? 3 : 2);
    if (result != map_page_status::ok) return result;
    if (bytes.size() != root_bytes ||
        get(bytes, bucketed ? 6 : 5, bucketed ? 2 : 3) || get(bytes, 40))
        return map_page_status::invalid;
    map_root root;
    root.bucketed = bucketed;
    root.grouped = bucketed && bytes[5] == 2;
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
        const auto keys = node.key.inline_value.size();
        const auto values = node.value.inline_value.size();
        char data[node_header_bytes + zset_key_inline_bytes + value_inline_bytes];
        std::memset(data, 0, node_header_bytes);
        std::memcpy(data, "PMN\2", 4);
        data[4] = static_cast<char>(node.bucket);
        put(data + 8, node.id); put(data + 16, node.left);
        put(data + 24, node.right); put(data + 32, node.height, 4);
        put(data + 40, node.elements);
        put(data + 48, node.key.length); put(data + 56, node.key.head);
        put(data + 64, node.value.length); put(data + 72, node.value.head);
        std::memcpy(data + node_header_bytes,
            node.key.inline_value.data(), keys);
        std::memcpy(data + node_header_bytes + keys,
            node.value.inline_value.data(), values);
        // Build first so out may alias either inline payload. assign retains
        // its strong failure guarantee while reusing an owned output buffer.
        out.assign(data, node_header_bytes + keys + values);
        return map_page_status::ok;
    } catch (const std::bad_alloc&) { return map_page_status::out_of_memory; }
}

map_page_status map_page_codec::encode_set_reference(const map_node& node,
    std::string& out) {
    if (!node.key.head || node.value.head || node.value.length ||
        (node.id | node.left | node.right | node.elements | node.key.head) > UINT16_MAX ||
        node.key.length > max_payload_bytes) return encode_node(node, out);
    if (!valid(node)) return map_page_status::invalid;
    try {
        char data[set_reference_bytes];
        std::memcpy(data, "PMN\22", 4);
        data[4] = static_cast<char>(node.bucket);
        data[5] = static_cast<char>(node.height);
        put(data + 6, node.id, 2); put(data + 8, node.left, 2);
        put(data + 10, node.right, 2); put(data + 12, node.elements, 2);
        put(data + 14, node.key.head, 2); put(data + 16, node.key.length, 4);
        std::string(data, sizeof(data)).swap(out);
        return map_page_status::ok;
    } catch (const std::bad_alloc&) { return map_page_status::out_of_memory; }
}

map_page_status map_page_codec::encode_packed_node(const map_node& node,
    std::string& out, bool hashed_bucket, bool compact_zset, bool compact_set_leaf) {
    return encode_packed_node_impl(node, out, hashed_bucket, false,
        compact_zset, compact_set_leaf);
}

map_page_status map_page_codec::encode_packed_node_scratch(const map_node& node,
    std::string& out, bool hashed_bucket, bool compact_zset) {
    return encode_packed_node_impl(node, out, hashed_bucket, true, compact_zset, false);
}

map_page_status map_page_codec::encode_packed_node_impl(const map_node& node,
    std::string& out, bool hashed_bucket, bool reuse_capacity, bool compact_zset,
    bool compact_set_leaf) {
    if (node.key.shares_fragments()) {
        if (hashed_bucket || node.bucket || !valid_header(node) ||
            node.key.head == node.id || node.key.inline_value.size() != 8 ||
            node.key.length <= zset_key_inline_bytes + 8 ||
            node.key.length > max_blob_bytes || node.value.head ||
            node.value.length || !node.value.inline_value.empty())
            return map_page_status::invalid;
        const auto fields = node.id | node.left | node.right | node.elements | node.key.head;
        const auto member_length = node.key.length - 8;
        if (compact_zset && fits_short_zset_reference(node, fields, member_length))
            return encode_short_zset_reference(node, true, member_length, out);
        const unsigned flags = fields <= UINT16_MAX ? 0 : fields <= UINT32_MAX ? 1 : 2;
        const auto width = 2U << flags;
        const auto header = zset_reference_header + 5 * width;
        char data[zset_reference_header + 5 * 8 + 8];
        std::memcpy(data, "PMN\24", 4);
        data[4] = static_cast<char>(flags);
        data[5] = static_cast<char>(node.height);
        data[6] = 0;
        put(data + 7, node.key.length, 4);
        auto* packed = data + zset_reference_header;
        if (flags == 0) {
            put(packed, (node.id << 48) | (node.left << 32) |
                (node.right << 16) | node.elements);
            put(packed + 8, node.key.head, 2);
        } else if (flags == 1) {
            put(packed, (node.id << 32) | node.left);
            put(packed + 8, (node.right << 32) | node.elements);
            put(packed + 16, node.key.head, 4);
        } else {
            put(packed, node.id);
            put(packed + 8, node.left);
            put(packed + 16, node.right);
            put(packed + 24, node.elements);
            put(packed + 32, node.key.head);
        }
        std::memcpy(data + header, node.key.inline_value.data(), 8);
        try {
            out.assign(data, header + 8);
            return map_page_status::ok;
        } catch (const std::bad_alloc&) { return map_page_status::out_of_memory; }
    }
    if (compact_zset && node.key.head && !node.value.head &&
        (hashed_bucket ? node.value.length == 8 && node.key.length <= max_payload_bytes :
            !node.value.length && !node.bucket)) {
        const auto fields = node.id | node.left | node.right | node.elements | node.key.head;
        if (fields > UINT32_MAX) return encode_node(node, out);
        if (!valid(node)) return map_page_status::invalid;
        if (hashed_bucket && fits_short_zset_reference(node, fields, node.key.length))
            return encode_short_zset_reference(node, false, node.key.length, out);
        try {
            const bool wide = fields > UINT16_MAX;
            const auto header = zset_reference_header + (wide ? 20 : 10);
            char data[zset_reference_header + 5 * 4 + 8];
            std::memcpy(data, "PMN\23", 4);
            data[4] = static_cast<char>((wide ? zset_reference_wide : 0) |
                (hashed_bucket ? zset_reference_valued : 0));
            data[5] = static_cast<char>(node.height);
            data[6] = static_cast<char>(node.bucket);
            put(data + 7, node.key.length, 4);
            auto* packed = data + zset_reference_header;
            if (wide) {
                put(packed, (node.id << 32) | node.left);
                put(packed + 8, (node.right << 32) | node.elements);
                put(packed + 16, node.key.head, 4);
            } else {
                put(packed, (node.id << 48) | (node.left << 32) |
                    (node.right << 16) | node.elements);
                put(packed + 8, node.key.head, 2);
            }
            std::memcpy(data + header, node.value.inline_value.data(), node.value.length);
            const auto size = header + node.value.length;
            if (size > std::string().capacity() && out.capacity() >= size && out.capacity() <= 64)
                out.assign(data, size);
            else
                std::string(data, size).swap(out);
            return map_page_status::ok;
        } catch (const std::bad_alloc&) { return map_page_status::out_of_memory; }
    }
    if (node.key.head || node.value.head || node.id > UINT32_MAX ||
        node.left > UINT32_MAX || node.right > UINT32_MAX ||
        node.elements > UINT32_MAX) return encode_node(node, out);
    if (!valid(node)) return map_page_status::invalid;
    try {
        const auto keys = node.key.inline_value.size();
        const auto values = node.value.inline_value.size();
        const bool leaf = !node.left && !node.right &&
            node.height == 1 && node.elements == 1;
        if (compact_set_leaf && leaf && hashed_bucket && !values &&
            keys >= set_short_min && keys <= set_short_min + 3 &&
            node.id <= set_short_id_mask) {
            char data[set_short_header + set_short_min + 3];
            data[0] = static_cast<char>(set_short_tag);
            put(data + 1, node.id | ((keys - set_short_min) << 14), 2);
            std::memcpy(data + set_short_header, node.key.inline_value.data(), keys);
            std::string(data, set_short_header + keys).swap(out);
            return map_page_status::ok;
        }
        const auto payload = keys + values;
        if (compact_zset && leaf && payload >= zset_wide_leaf_min &&
            node.id <= zset_leaf_id_mask &&
            (hashed_bucket ? values == 8 : !values && !node.bucket)) {
            char data[zset_wide_leaf_header + zset_key_inline_bytes + 8];
            data[0] = static_cast<char>(zset_wide_leaf_tag);
            put(data + 1, node.id | (hashed_bucket ? zset_leaf_valued : 0), 2);
            std::memcpy(data + zset_wide_leaf_header, node.key.inline_value.data(), keys);
            std::memcpy(data + zset_wide_leaf_header + keys, node.value.inline_value.data(), values);
            const auto size = zset_wide_leaf_header + payload;
            if (out.capacity() == size) out.assign(data, size);
            else std::string(data, size).swap(out);
            return map_page_status::ok;
        }
        const bool short_header = payload > zset_leaf_max;
        if (!reuse_capacity && compact_zset && leaf && payload >= zset_leaf_min &&
            payload <= zset_short_payload + 1 &&
            node.id <= (short_header ? zset_short_id_mask : zset_leaf_id_mask) &&
            (hashed_bucket ? values == 8 : !values && !node.bucket)) {
            char data[zset_leaf_header + zset_leaf_max];
            const auto meta = node.id | (hashed_bucket ? zset_leaf_valued : 0);
            const auto header = short_header ? zset_short_header : zset_leaf_header;
            if (short_header) {
                put(data, zset_short_signature, 2);
                put(data + 2, meta | (payload > zset_short_payload ? zset_short_length : 0), 2);
            } else {
                std::memcpy(data, "PMN\21", 4);
                put(data + 4, meta, 2);
            }
            std::memcpy(data + header, node.key.inline_value.data(), keys);
            std::memcpy(data + header + keys, node.value.inline_value.data(), values);
            const auto size = header + payload;
            const auto local = std::string().capacity();
            const auto limit = size <= local ? local : sizeof(data);
            if (out.capacity() >= size && out.capacity() <= limit)
                out.assign(data, size);
            else
                std::string(data, size).swap(out);
            return map_page_status::ok;
        }
        if (hashed_bucket && leaf && !values && node.id <= UINT16_MAX &&
            keys > short_leaf_payload && keys <= hashed_key_leaf_max) {
            char data[hashed_key_leaf_header + hashed_key_leaf_max];
            std::memcpy(data, "PMN\20", 4);
            put(data + 4, node.id, 2);
            data[6] = static_cast<char>(node.bucket);
            std::memcpy(data + hashed_key_leaf_header, node.key.inline_value.data(), keys);
            const auto size = hashed_key_leaf_header + keys;
            if (out.capacity() >= size && out.capacity() <= hashed_key_leaf_max + hashed_key_leaf_header)
                out.assign(data, size);
            else
                std::string(data, size).swap(out);
            return map_page_status::ok;
        }
        // Larger key-only branches have only the compact member or medium
        // layout when their links and counts fit in 16 bits.
        if (!values && keys >= compact_member_min && (node.left || node.right) &&
            (keys > compact_member_max ||
                (node.id | node.left | node.right) > compact_member_id_mask ||
                node.height > compact_member_height_max ||
                node.elements > compact_member_count_max) &&
            node.id <= UINT16_MAX &&
            node.left <= UINT16_MAX && node.right <= UINT16_MAX &&
            node.elements <= UINT16_MAX) {
            char data[medium_node_header + zset_key_inline_bytes];
            std::memcpy(data, "PMN\11", 4);
            data[4] = static_cast<char>(node.bucket);
            data[5] = static_cast<char>(node.height);
            data[6] = static_cast<char>(keys);
            data[7] = 0;
            put(data + 8, (node.id << 48) | (node.left << 32) |
                (node.right << 16) | node.elements);
            std::memcpy(data + medium_node_header, node.key.inline_value.data(), keys);
            out.assign(data, medium_node_header + keys);
            return map_page_status::ok;
        }
        const bool short_score_leaf = values == 8 && keys >= 7 && keys <= 14;
        if (leaf && node.id > UINT16_MAX &&
            (hashed_bucket ? (values && keys + values >= compact_hash_leaf_min) ||
                short_score_leaf : !values && !node.bucket && keys > short_leaf_payload)) {
            char data[wide_leaf_header + zset_key_inline_bytes + value_inline_bytes];
            std::memcpy(data, "PMN\16", 4);
            put(data + 4, node.id, 4);
            data[8] = static_cast<char>(keys |
                (hashed_bucket ? wide_leaf_hashed : 0));
            std::memcpy(data + wide_leaf_header, node.key.inline_value.data(), keys);
            std::memcpy(data + wide_leaf_header + keys,
                node.value.inline_value.data(), values);
            const auto size = wide_leaf_header + keys + values;
            // The backend copies these staged pages into its own buffers.
            // Reuse a larger scratch allocation for short score leaves.
            if (out.capacity() >= size && (short_score_leaf || out.capacity() - size <= 15))
                out.assign(data, size);
            else
                std::string(data, size).swap(out);
            return map_page_status::ok;
        }
        if (leaf && values && keys + values >= compact_hash_leaf_min &&
            keys + values <= compact_hash_leaf_max && node.id <= UINT16_MAX &&
            (hashed_bucket || node.bucket == scan_bucket(node.key.inline_value))) {
            char data[compact_hash_leaf_header + compact_hash_leaf_max];
            std::memcpy(data, "PMN\14", 4);
            put(data + 4, node.id, 2);
            data[6] = static_cast<char>(keys);
            std::memcpy(data + compact_hash_leaf_header,
                node.key.inline_value.data(), keys);
            std::memcpy(data + compact_hash_leaf_header + keys,
                node.value.inline_value.data(), values);
            const auto size = compact_hash_leaf_header + keys + values;
            if (reuse_capacity && out.capacity() >= size && out.capacity() <= 128)
                out.assign(data, size);
            else
                std::string(data, size).swap(out);
            return map_page_status::ok;
        }
        if (leaf && keys + values <= leaf_inline_bytes) {
            char data[leaf_node_header + leaf_inline_bytes];
            std::memcpy(data, "PMN\4", 4);
            data[4] = static_cast<char>(node.bucket);
            data[5] = static_cast<char>(keys);
            data[6] = static_cast<char>(values);
            put(data + 7, node.id, 4);
            std::memcpy(data + leaf_node_header, node.key.inline_value.data(), keys);
            std::memcpy(data + leaf_node_header + keys,
                node.value.inline_value.data(), values);
            out.assign(data, leaf_node_header + keys + values);
            return map_page_status::ok;
        }
        if (leaf && node.id <= UINT16_MAX && keys + values <= short_leaf_payload) {
            char data[short_leaf_header + short_leaf_payload];
            std::memcpy(data, "PMN\7", 4);
            data[4] = static_cast<char>(node.bucket);
            data[5] = static_cast<char>((keys << 4) | values);
            put(data + 6, node.id, 2);
            std::memcpy(data + short_leaf_header, node.key.inline_value.data(), keys);
            std::memcpy(data + short_leaf_header + keys,
                node.value.inline_value.data(), values);
            out.assign(data, short_leaf_header + keys + values);
            return map_page_status::ok;
        }
        if (node.id <= UINT8_MAX && node.left <= UINT8_MAX &&
            node.right <= UINT8_MAX && node.elements <= UINT8_MAX &&
            keys + values <= leaf_inline_bytes) {
            char data[small_node_header + leaf_inline_bytes];
            std::memcpy(data, "PMN\5", 4);
            data[4] = static_cast<char>(node.bucket);
            data[5] = static_cast<char>((keys << 4) | values);
            data[6] = static_cast<char>(node.height);
            data[7] = static_cast<char>(node.id);
            data[8] = static_cast<char>(node.left);
            data[9] = static_cast<char>(node.right);
            data[10] = static_cast<char>(node.elements);
            std::memcpy(data + small_node_header, node.key.inline_value.data(), keys);
            std::memcpy(data + small_node_header + keys,
                node.value.inline_value.data(), values);
            out.assign(data, small_node_header + keys + values);
            return map_page_status::ok;
        }
        if (node.id <= UINT8_MAX && node.left <= UINT8_MAX &&
            node.right <= UINT8_MAX && node.elements <= UINT8_MAX &&
            node.height <= 15 && keys + values == narrow_payload) {
            char data[narrow_node_header + narrow_payload];
            std::memcpy(data, "PMN\6", 4);
            data[4] = static_cast<char>(node.bucket);
            data[5] = static_cast<char>((keys << 4) | node.height);
            data[6] = static_cast<char>(node.id);
            data[7] = static_cast<char>(node.left);
            data[8] = static_cast<char>(node.right);
            data[9] = static_cast<char>(node.elements);
            std::memcpy(data + narrow_node_header, node.key.inline_value.data(), keys);
            std::memcpy(data + narrow_node_header + keys,
                node.value.inline_value.data(), values);
            out.assign(data, sizeof(data));
            return map_page_status::ok;
        }
        const bool valued_branch = hashed_bucket && keys == valued_branch_key &&
            values == valued_branch_value;
        if (((keys == short_branch_payload && values == 0 && node.bucket == 0) ||
             valued_branch) &&
            node.id <= UINT8_MAX && node.left <= UINT8_MAX &&
            node.right <= UINT8_MAX && (node.left || node.right) &&
            node.height >= 2 && node.height <= short_branch_max_height &&
            node.elements >= 2 && node.elements <= short_branch_max_elements) {
            char data[short_branch_header + short_branch_payload];
            std::memcpy(data, "PMN", 3);
            data[3] = valued_branch ? 13 : 8;
            data[4] = static_cast<char>(((node.height - 2) << 5) | (node.elements - 1));
            data[5] = static_cast<char>(node.id);
            data[6] = static_cast<char>(node.left);
            data[7] = static_cast<char>(node.right);
            std::memcpy(data + short_branch_header, node.key.inline_value.data(), keys);
            std::memcpy(data + short_branch_header + keys,
                node.value.inline_value.data(), values);
            out.assign(data, sizeof(data));
            return map_page_status::ok;
        }
        if (values == 0 && keys >= compact_member_min &&
            keys <= compact_member_max && node.id <= compact_member_id_mask &&
            node.left <= compact_member_id_mask && node.right <= compact_member_id_mask &&
            node.height <= compact_member_height_max &&
            node.elements <= compact_member_count_max) {
            char data[compact_member_header + compact_member_max];
            std::memcpy(data, "PMN\13", 4);
            data[4] = static_cast<char>(node.bucket);
            data[5] = static_cast<char>(((node.height - 1) << 5) | (node.elements - 1));
            put(data + 6, (node.id << 24) | (node.left << 12) | node.right, 5);
            std::memcpy(data + compact_member_header, node.key.inline_value.data(), keys);
            // Construct at the final size. Growing an empty string can
            // allocate a larger size class than the compact record needs.
            const auto size = compact_member_header + keys;
            if (reuse_capacity && out.capacity() >= size && out.capacity() <= 128)
                out.assign(data, size);
            else
                std::string(data, size).swap(out);
            return map_page_status::ok;
        }
        char data[packed_node_header + zset_key_inline_bytes + value_inline_bytes];
        const bool medium = node.id <= UINT16_MAX && node.left <= UINT16_MAX &&
            node.right <= UINT16_MAX && node.elements <= UINT16_MAX;
        const bool wide24 = !medium && keys + values >= 24 &&
            node.id <= wide24_id_max && node.left <= wide24_id_max &&
            node.right <= wide24_id_max && node.elements <= UINT16_MAX;
        const auto header = medium ? medium_node_header :
            wide24 ? wide24_node_header : packed_node_header;
        std::memcpy(data, "PMN", 3);
        data[3] = medium ? 9 : wide24 ? 15 : 3;
        data[4] = static_cast<char>(node.bucket);
        data[5] = static_cast<char>(node.height);
        data[6] = static_cast<char>(keys);
        data[7] = static_cast<char>(values);
        if (medium) {
            put(data + 8, node.id, 2); put(data + 10, node.left, 2);
            put(data + 12, node.right, 2); put(data + 14, node.elements, 2);
        } else if (wide24) {
            put(data + 8, node.id, 3); put(data + 11, node.left, 3);
            put(data + 14, node.right, 3); put(data + 17, node.elements, 2);
        } else {
            put(data + 8, node.id, 4); put(data + 12, node.left, 4);
            put(data + 16, node.right, 4); put(data + 20, node.elements, 4);
        }
        std::memcpy(data + header, node.key.inline_value.data(), keys);
        std::memcpy(data + header + keys, node.value.inline_value.data(), values);
        const auto size = header + keys + values;
        if (wide24 && (out.capacity() < size || out.capacity() - size > 15))
            std::string(data, size).swap(out);
        else
            out.assign(data, size);
        return map_page_status::ok;
    } catch (const std::bad_alloc&) { return map_page_status::out_of_memory; }
}

map_page_status map_page_codec::decode_node(const std::string& bytes,
    map_node& out) {
    map_node node;
    const auto result = decode_node_scratch(bytes, node);
    if (result == map_page_status::ok) out = std::move(node);
    return result;
}

map_page_status map_page_codec::decode_node_scratch(const std::string& bytes,
    map_node& node, bool with_value) {
    if (&bytes == &node.key.inline_value || &bytes == &node.value.inline_value) {
        map_node scratch;
        const auto status = decode_node_scratch(bytes, scratch, with_value);
        if (status == map_page_status::ok) node = std::move(scratch);
        return status;
    }
    size_t header;
    const auto result = read_node_header(bytes, node, header);
    if (result != map_page_status::ok) return result;
    try {
        const auto keys = node.key.head ? (shares_zset_reference(bytes) ? 8 : 0) : node.key.length;
        const auto values = !with_value || node.value.head ? 0 : node.value.length;
        const char* payload = bytes.data() + header;
        // A constant length lets the library specialize short assignment
        // while preserving any capacity already held by the scratch node.
        if (keys == 8) node.key.inline_value.assign(payload, 8);
        else if (keys) node.key.inline_value.assign(payload, static_cast<size_t>(keys));
        else node.key.inline_value.clear();
        if (values == 8) node.value.inline_value.assign(payload + keys, 8);
        else if (values) node.value.inline_value.assign(payload + keys, static_cast<size_t>(values));
        else node.value.inline_value.clear();
        return map_page_status::ok;
    } catch (const std::bad_alloc&) { return map_page_status::out_of_memory; }
}

map_page_status map_page_codec::decode_node_view(const std::string& bytes,
    map_node_view& out) {
    map_node_view node;
    const auto result = decode_node_view_scratch(bytes, node);
    if (result == map_page_status::ok) out = node;
    return result;
}

map_page_status map_page_codec::decode_node_view_scratch(
    const std::string& bytes, map_node_view& node) {
    size_t header;
    const auto result = read_node_header(bytes, node, header);
    if (result != map_page_status::ok) return result;
    node.key.shared = node.key.head && shares_zset_reference(bytes);
    node.value.shared = false;
    node.key.data = bytes.data() + header;
    node.value.data = node.key.data + node.key.inline_size();
    return map_page_status::ok;
}

map_page_status map_page_codec::encode_fragment(const map_fragment& page,
    std::string& out) {
    return encode_fragment(page.id, page.next, page.offset, page.bytes.data(),
        page.bytes.size(), out);
}

map_page_status map_page_codec::encode_fragment(uint64_t id, uint64_t next,
    uint64_t offset, const char* payload, size_t size, std::string& out,
    bool compact_single) {
    if (!valid_fragment(id, next, offset, size)) return map_page_status::invalid;
    try {
        std::string bytes;
        if (compact_single && !next && !offset && id <= UINT32_MAX &&
            size <= 512) {
            char header[8];
            std::memcpy(header, "PMB\2", 4);
            put(header + 4, id, 4);
            bytes.reserve(sizeof(header) + size);
            bytes.append(header, sizeof(header));
            bytes.append(payload, size);
            out.swap(bytes);
            return map_page_status::ok;
        }
        bytes.reserve(fragment_header_bytes + size);
        bytes.resize(fragment_header_bytes);
        char* data = &bytes[0];
        std::memcpy(data, "PMB\1", 4);
        put(data + 8, id); put(data + 16, next);
        put(data + 24, offset);
        bytes.append(payload, size);
        out.swap(bytes);
        return map_page_status::ok;
    } catch (const std::bad_alloc&) { return map_page_status::out_of_memory; }
}

map_page_status map_page_codec::decode_fragment(const std::string& bytes,
    map_fragment& out) {
    map_fragment_view view;
    const auto result = decode_fragment_view(bytes, view);
    if (result != map_page_status::ok) return result;
    try {
        map_fragment page;
        page.id = view.id; page.next = view.next; page.offset = view.offset;
        page.bytes.assign(view.data, view.size);
        out = std::move(page);
        return map_page_status::ok;
    } catch (const std::bad_alloc&) { return map_page_status::out_of_memory; }
}

map_page_status map_page_codec::decode_fragment_view(const std::string& bytes,
    map_fragment_view& out) {
    if (bytes.size() >= 4 && std::memcmp(bytes.data(), "PMB\2", 4) == 0) {
        constexpr size_t header = 8;
        if (bytes.size() <= header || bytes.size() - header > 512)
            return map_page_status::invalid;
        map_fragment_view page;
        page.id = get(bytes, 4, 4);
        if (!page.id) return map_page_status::invalid;
        page.data = bytes.data() + header;
        page.size = bytes.size() - header;
        out = page;
        return map_page_status::ok;
    }
    const auto result = signature(bytes, "PMB");
    if (result != map_page_status::ok) return result;
    if (bytes.size() <= fragment_header_bytes ||
        bytes.size() > max_page_bytes ||
        get(bytes, 4, 4)) return map_page_status::invalid;
    map_fragment_view page;
    page.id = get(bytes, 8); page.next = get(bytes, 16);
    page.offset = get(bytes, 24);
    page.data = bytes.data() + fragment_header_bytes;
    page.size = bytes.size() - fragment_header_bytes;
    if (!valid_fragment(page.id, page.next, page.offset, page.size))
        return map_page_status::invalid;
    out = page;
    return map_page_status::ok;
}

} // namespace pkv
