#include "zset_page.h"
#include <cmath>
#include <cstring>
#include <limits>
#include <new>

namespace pkv {

const size_t zset_page_codec::root_bytes;
namespace {
void put(char* out, uint64_t value) {
#if defined(__GNUC__) && defined(__BYTE_ORDER__) && \
    __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    value = __builtin_bswap64(value);
    std::memcpy(out, &value, sizeof(value));
#elif defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    std::memcpy(out, &value, sizeof(value));
#else
    for (unsigned i = 0; i < sizeof(value); ++i)
        out[i] = static_cast<char>(value >> ((sizeof(value) - i - 1) * 8));
#endif
}
uint64_t score_bits(double score) {
    static_assert(sizeof(double) == sizeof(uint64_t) &&
        std::numeric_limits<double>::is_iec559, "IEEE-754 double required");
    uint64_t bits;
    std::memcpy(&bits, &score, sizeof(bits));
    return bits >> 63 ? ~bits : bits ^ (uint64_t(1) << 63);
}
uint64_t get(const char* bytes) {
#if defined(__GNUC__) && defined(__BYTE_ORDER__) && \
    __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    uint64_t value;
    std::memcpy(&value, bytes, sizeof(value));
    return __builtin_bswap64(value);
#elif defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    uint64_t value;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
#else
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i)
        value = (value << 8) | static_cast<unsigned char>(bytes[i]);
    return value;
#endif
}
uint64_t get(const std::string& bytes, size_t offset) {
    return get(bytes.data() + offset);
}
bool valid(const zset_root& root) {
    if (root.expires_at_ms < -1 || root.elements > static_cast<uint64_t>(
        std::numeric_limits<int64_t>::max())) return false;
    if (!root.elements) return !root.members && !root.scores;
    return root.members && root.scores && root.members != root.scores &&
        root.members <= root.last_id && root.scores <= root.last_id &&
        root.elements <= root.last_id / 2;
}

// Compact roots use four 16-bit counters. The deadline is stored only
// when present, and its flag distinguishes a truncated expiring root.
constexpr size_t compact_root_bytes = 13;
constexpr size_t compact_expiring_root_bytes = compact_root_bytes + 8;
void put_short(char* out, uint64_t value) {
    out[0] = static_cast<char>(value >> 8);
    out[1] = static_cast<char>(value);
}
uint64_t get_short(const std::string& bytes, size_t offset) {
    return (uint64_t(static_cast<unsigned char>(bytes[offset])) << 8) |
        static_cast<unsigned char>(bytes[offset + 1]);
}
}

map_page_status zset_page_codec::encode_root(const zset_root& root,
    std::string& out) {
    if (!valid(root)) return map_page_status::invalid;
    try {
        if (root.last_id <= UINT16_MAX) {
            char bytes[compact_expiring_root_bytes];
            std::memcpy(bytes, "PZR\2", 4);
            const bool expiring = root.expires_at_ms >= 0;
            bytes[4] = expiring;
            put_short(bytes + 5, root.elements);
            put_short(bytes + 7, root.members);
            put_short(bytes + 9, root.scores);
            put_short(bytes + 11, root.last_id);
            if (expiring) put(bytes + compact_root_bytes, root.expires_at_ms);
            out.assign(bytes, expiring ? sizeof(bytes) : compact_root_bytes);
            return map_page_status::ok;
        }
        char bytes[root_bytes] = {};
        std::memcpy(bytes, "PZR\1", 4);
        put(bytes + 8, static_cast<uint64_t>(root.expires_at_ms));
        put(bytes + 16, root.elements);
        put(bytes + 24, root.members);
        put(bytes + 32, root.scores);
        put(bytes + 40, root.last_id);
        out.assign(bytes, sizeof(bytes));
        return map_page_status::ok;
    } catch (const std::bad_alloc&) { return map_page_status::out_of_memory; }
}

map_page_status zset_page_codec::decode_root(const std::string& bytes,
    zset_root& out) {
    if (bytes.size() < 4 || std::memcmp(bytes.data(), "PZR", 3) != 0)
        return map_page_status::invalid;
    zset_root root;
    uint64_t expiry = UINT64_MAX;
    if (bytes[3] == 1) {
        if (bytes.size() != root_bytes || bytes[4] || bytes[5] || bytes[6] ||
            bytes[7] || get(bytes, 48) || get(bytes, 56))
            return map_page_status::invalid;
        expiry = get(bytes, 8);
        root.elements = get(bytes, 16); root.members = get(bytes, 24);
        root.scores = get(bytes, 32); root.last_id = get(bytes, 40);
    } else if (bytes[3] == 2) {
        if (bytes.size() < compact_root_bytes || (bytes[4] != 0 && bytes[4] != 1))
            return map_page_status::invalid;
        const bool expiring = bytes[4] == 1;
        if (bytes.size() != (expiring ? compact_expiring_root_bytes :
            compact_root_bytes)) return map_page_status::invalid;
        if (expiring) {
            expiry = get(bytes, compact_root_bytes);
            if (expiry > static_cast<uint64_t>(INT64_MAX))
                return map_page_status::invalid;
        }
        root.elements = get_short(bytes, 5); root.members = get_short(bytes, 7);
        root.scores = get_short(bytes, 9); root.last_id = get_short(bytes, 11);
    } else return map_page_status::unsupported;
    if (expiry != UINT64_MAX && expiry > static_cast<uint64_t>(INT64_MAX))
        return map_page_status::invalid;
    root.expires_at_ms = expiry == UINT64_MAX ? -1 :
        static_cast<int64_t>(expiry);
    if (!valid(root)) return map_page_status::invalid;
    out = root;
    return map_page_status::ok;
}

bool zset_page_codec::encode_score(double score, std::string& out) {
    if (score == 0) score = 0;
    return encode_value(score, out);
}

bool zset_page_codec::encode_value(double score, std::string& out) {
    if (std::isnan(score)) return false;
    char bytes[8];
    put(bytes, score_bits(score));
    out.assign(bytes, sizeof(bytes));
    return true;
}

bool zset_page_codec::append_value(double score, std::string& out) {
    if (std::isnan(score)) return false;
    char bytes[8];
    put(bytes, score_bits(score));
    out.append(bytes, sizeof(bytes));
    return true;
}

bool zset_page_codec::decode_score(const std::string& bytes, double& out) {
    return bytes.size() == 8 && decode_score_prefix(bytes, out);
}

static bool decode_score_word(uint64_t bits, double& out) {
    bits = bits >> 63 ? bits ^ (uint64_t(1) << 63) : ~bits;
    double score;
    std::memcpy(&score, &bits, sizeof(score));
    if (std::isnan(score)) return false;
    out = score;
    return true;
}

bool zset_page_codec::decode_score_prefix(const std::string& key, double& out) {
    double score;
    if (key.size() < 8 || !decode_score_word(get(key, 0), score) ||
        (score == 0 && std::signbit(score))) return false;
    out = score;
    return true;
}

bool zset_page_codec::decode_value(const std::string& bytes, double& out) {
    return decode_value(bytes.data(), bytes.size(), out);
}

bool zset_page_codec::decode_value(const char* bytes, size_t length, double& out) {
    return length == 8 && decode_score_word(get(bytes), out);
}

bool zset_page_codec::score_key(double score, const std::string& member,
    std::string& out) {
    if (member.size() > map_page_codec::max_payload_bytes || std::isnan(score))
        return false;
    std::string temporary;
    auto& bytes = &member == &out ? temporary : out;
    bytes.reserve(8 + member.size());
    char prefix[8];
    put(prefix, score_bits(score == 0 ? 0 : score));
    bytes.assign(prefix, sizeof(prefix));
    bytes += member;
    if (&bytes != &out) out.swap(bytes);
    return true;
}

} // namespace pkv
