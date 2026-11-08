#include "map_page.h"
#include <cstring>
#include <limits>
#include <new>

namespace pkv {
const size_t map_page_codec::directory_bytes;
namespace {
void put(char* p, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i)
        p[i] = static_cast<char>(value >> (56 - i * 8));
}
uint64_t get(const char* p) {
    uint64_t result = 0;
    for (unsigned i = 0; i < 8; ++i)
        result = (result << 8) | static_cast<unsigned char>(p[i]);
    return result;
}
bool valid(const map_root& root, const map_directory& directory) {
    if (!root.bucketed || root.grouped || root.kind != map_kind::hash ||
        root.expires_at_ms < -1 || !root.tree ||
        root.tree > root.last_id || !root.elements || root.elements >
            static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
        return false;
    std::array<uint64_t, 512> seen{};
    uint64_t total = 0;
    for (const auto& entry : directory.buckets) {
        if ((entry.elements == 0) != (entry.tree == 0) ||
            entry.tree > root.last_id || entry.tree == root.tree ||
            entry.elements > root.elements - total) return false;
        total += entry.elements;
        if (!entry.tree) continue;
        size_t slot = static_cast<size_t>((entry.tree *
            UINT64_C(11400714819323198485)) >> 55);
        while (seen[slot]) {
            if (seen[slot] == entry.tree) return false;
            slot = (slot + 1) & 511;
        }
        seen[slot] = entry.tree;
    }
    return total == root.elements;
}
} // namespace

map_page_status map_page_codec::encode_directory(const map_root& root,
    const map_directory& directory, std::string& out) {
    if (!valid(root, directory)) return map_page_status::invalid;
    try {
        std::string bytes(directory_bytes, '\0');
        std::memcpy(&bytes[0], "PMD\1", 4);
        put(&bytes[8], root.tree);
        for (size_t i = 0; i < directory.buckets.size(); ++i) {
            put(&bytes[16 + 16 * i], directory.buckets[i].tree);
            put(&bytes[24 + 16 * i], directory.buckets[i].elements);
        }
        out.swap(bytes);
        return map_page_status::ok;
    } catch (const std::bad_alloc&) { return map_page_status::out_of_memory; }
}

map_page_status map_page_codec::decode_directory(const std::string& bytes,
    const map_root& root, map_directory& out) {
    if (bytes.size() < 4 || bytes.compare(0, 3, "PMD") != 0)
        return map_page_status::invalid;
    if (bytes[3] != 1) return map_page_status::unsupported;
    if (bytes.size() != directory_bytes || get(bytes.data() + 8) != root.tree
        || std::memcmp(bytes.data() + 4, "\0\0\0\0", 4) != 0)
        return map_page_status::invalid;
    map_directory directory;
    for (size_t i = 0; i < directory.buckets.size(); ++i) {
        directory.buckets[i].tree = get(bytes.data() + 16 + 16 * i);
        directory.buckets[i].elements = get(bytes.data() + 24 + 16 * i);
    }
    if (!valid(root, directory)) return map_page_status::invalid;
    out = directory;
    return map_page_status::ok;
}
} // namespace pkv
