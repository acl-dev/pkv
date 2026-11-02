#include "zset_page.h"
#include <cmath>
#include <cstring>
#include <limits>
#include <new>

namespace pkv {

const size_t zset_page_codec::root_bytes;
namespace {
void put(std::string& out, uint64_t value) {
    for (unsigned i = 8; i; --i)
        out.push_back(static_cast<char>(value >> ((i - 1) * 8)));
}
uint64_t get(const std::string& bytes, size_t offset) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i)
        value = (value << 8) | static_cast<unsigned char>(bytes[offset + i]);
    return value;
}
bool valid(const zset_root& root) {
    if (root.expires_at_ms < -1 || root.elements > static_cast<uint64_t>(
        std::numeric_limits<int64_t>::max())) return false;
    if (!root.elements) return !root.members && !root.scores;
    return root.members && root.scores && root.members != root.scores &&
        root.members <= root.last_id && root.scores <= root.last_id &&
        root.elements <= root.last_id / 2;
}
}

map_page_status zset_page_codec::encode_root(const zset_root& root,
    std::string& out) {
    if (!valid(root)) return map_page_status::invalid;
    try {
        std::string bytes("PZR\1", 4);
        bytes.append(4, '\0');
        put(bytes, static_cast<uint64_t>(root.expires_at_ms));
        put(bytes, root.elements); put(bytes, root.members);
        put(bytes, root.scores); put(bytes, root.last_id);
        put(bytes, 0); put(bytes, 0);
        out.swap(bytes);
        return map_page_status::ok;
    } catch (const std::bad_alloc&) { return map_page_status::out_of_memory; }
}

map_page_status zset_page_codec::decode_root(const std::string& bytes,
    zset_root& out) {
    if (bytes.size() < 4 || bytes.compare(0, 3, "PZR") != 0)
        return map_page_status::invalid;
    if (bytes[3] != 1) return map_page_status::unsupported;
    if (bytes.size() != root_bytes || bytes[4] || bytes[5] || bytes[6] ||
        bytes[7] || get(bytes, 48) || get(bytes, 56))
        return map_page_status::invalid;
    zset_root root;
    const auto expiry = get(bytes, 8);
    if (expiry != UINT64_MAX && expiry > static_cast<uint64_t>(INT64_MAX))
        return map_page_status::invalid;
    root.expires_at_ms = expiry == UINT64_MAX ? -1 :
        static_cast<int64_t>(expiry);
    root.elements = get(bytes, 16); root.members = get(bytes, 24);
    root.scores = get(bytes, 32); root.last_id = get(bytes, 40);
    if (!valid(root)) return map_page_status::invalid;
    out = root;
    return map_page_status::ok;
}

bool zset_page_codec::encode_score(double score, std::string& out) {
    if (score == 0) score = 0;
    return encode_value(score, out);
}

bool zset_page_codec::encode_value(double score, std::string& out) {
    static_assert(sizeof(double) == sizeof(uint64_t) &&
        std::numeric_limits<double>::is_iec559, "IEEE-754 double required");
    if (std::isnan(score)) return false;
    uint64_t bits;
    std::memcpy(&bits, &score, sizeof(bits));
    bits = bits >> 63 ? ~bits : bits ^ (uint64_t(1) << 63);
    std::string bytes;
    put(bytes, bits); out.swap(bytes);
    return true;
}

bool zset_page_codec::decode_score(const std::string& bytes, double& out) {
    double score;
    if (!decode_value(bytes, score) || (score == 0 && std::signbit(score)))
        return false;
    out = score;
    return true;
}

bool zset_page_codec::decode_value(const std::string& bytes, double& out) {
    if (bytes.size() != 8) return false;
    uint64_t bits = get(bytes, 0);
    bits = bits >> 63 ? bits ^ (uint64_t(1) << 63) : ~bits;
    double score;
    std::memcpy(&score, &bits, sizeof(score));
    if (std::isnan(score)) return false;
    out = score;
    return true;
}

bool zset_page_codec::score_key(double score, const std::string& member,
    std::string& out) {
    if (member.size() > map_page_codec::max_payload_bytes) return false;
    std::string bytes;
    if (!encode_score(score, bytes)) return false;
    bytes += member; out.swap(bytes);
    return true;
}

} // namespace pkv
