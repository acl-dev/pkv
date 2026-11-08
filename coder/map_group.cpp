#include "map_page.h"
#include <cstring>
#include <limits>
#include <new>

namespace pkv {
const size_t map_page_codec::group_bytes;
namespace {
constexpr size_t compact_group_bytes = 8 + 16 * 4;
constexpr size_t sparse_group_header = 10;
void put_short(char* out, uint64_t value) {
    out[0] = static_cast<char>(value >> 8);
    out[1] = static_cast<char>(value);
}
uint16_t get_short(const char* in) {
    return (static_cast<uint8_t>(in[0]) << 8) | static_cast<uint8_t>(in[1]);
}
void put(char* out, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i)
        out[i] = static_cast<char>(value >> (56 - i * 8));
}
uint64_t get(const char* in) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i)
        value = (value << 8) | static_cast<unsigned char>(in[i]);
    return value;
}
bool valid(const map_root& root, unsigned tag, const map_group& group) {
    if (!root.bucketed || !root.grouped ||
        (root.kind != map_kind::hash && root.kind != map_kind::set) ||
        root.expires_at_ms < -1 || !root.tree || !root.elements ||
        root.tree > root.last_id || (tag >= 16 && tag != 256) ||
        root.elements > static_cast<uint64_t>(
            std::numeric_limits<int64_t>::max())) return false;
    std::array<uint64_t, 32> seen{};
    uint64_t total = 0;
    for (const auto& entry : group.buckets) {
        if ((entry.tree == 0) != (entry.elements == 0) ||
            entry.tree > root.last_id || entry.tree == root.tree ||
            entry.elements > root.elements - total) return false;
        total += entry.elements;
        if (!entry.tree) continue;
        size_t slot = static_cast<size_t>((entry.tree *
            UINT64_C(11400714819323198485)) >> 59);
        while (seen[slot]) {
            if (seen[slot] == entry.tree) return false;
            slot = (slot + 1) & 31;
        }
        seen[slot] = entry.tree;
    }
    return total == root.elements;
}
} // namespace

map_page_status map_page_codec::encode_group(const map_root& root,
    unsigned tag, const map_group& group, std::string& out) {
    if (!valid(root, tag, group)) return map_page_status::invalid;
    try {
        if (root.last_id <= UINT16_MAX && root.elements <= UINT16_MAX) {
            uint16_t present = 0;
            bool byte_counts = true;
            for (size_t i = 0; i < 16; ++i) {
                const auto& entry = group.buckets[i];
                if (entry.elements > UINT8_MAX) byte_counts = false;
                if (entry.tree) present |= uint16_t(1) << i;
            }
            if (byte_counts) {
                char bytes[sparse_group_header + 16 * 3];
                std::memcpy(bytes, "PMD\4", 4);
                put_short(bytes + 4, tag);
                put_short(bytes + 6, root.tree);
                put_short(bytes + 8, present);
                auto* next = bytes + sparse_group_header;
                for (const auto& entry : group.buckets) {
                    if (!entry.tree) continue;
                    put_short(next, entry.tree);
                    next[2] = static_cast<char>(entry.elements);
                    next += 3;
                }
                std::string encoded(bytes, static_cast<size_t>(next - bytes));
                out.swap(encoded);
                return map_page_status::ok;
            }
            char bytes[compact_group_bytes];
            std::memcpy(bytes, "PMD\3", 4);
            put_short(bytes + 4, tag);
            put_short(bytes + 6, root.tree);
            for (size_t i = 0; i < 16; ++i) {
                put_short(bytes + 8 + i * 4, group.buckets[i].tree);
                put_short(bytes + 10 + i * 4, group.buckets[i].elements);
            }
            out.assign(bytes, sizeof(bytes));
            return map_page_status::ok;
        }
        std::string bytes(group_bytes, '\0');
        std::memcpy(&bytes[0], "PMD\2", 4);
        bytes[4] = static_cast<char>(tag >> 8);
        bytes[5] = static_cast<char>(tag);
        put(&bytes[8], root.tree);
        for (size_t i = 0; i < 16; ++i) {
            put(&bytes[16 + i * 16], group.buckets[i].tree);
            put(&bytes[24 + i * 16], group.buckets[i].elements);
        }
        out.swap(bytes);
        return map_page_status::ok;
    } catch (const std::bad_alloc&) { return map_page_status::out_of_memory; }
}

map_page_status map_page_codec::decode_group(const std::string& bytes,
    const map_root& root, unsigned tag, map_group& out) {
    if (bytes.size() < 4 || bytes.compare(0, 3, "PMD") != 0)
        return map_page_status::invalid;
    if (bytes[3] != 2 && bytes[3] != 3 && bytes[3] != 4)
        return map_page_status::unsupported;
    map_group group;
    if (bytes[3] == 4) {
        if (bytes.size() < sparse_group_header ||
            get_short(bytes.data() + 4) != tag ||
            get_short(bytes.data() + 6) != root.tree)
            return map_page_status::invalid;
        const auto present = get_short(bytes.data() + 8);
        size_t offset = sparse_group_header;
        for (unsigned i = 0; i < 16; ++i) {
            if (!(present & (uint16_t(1) << i))) continue;
            if (bytes.size() - offset < 3) return map_page_status::invalid;
            auto& entry = group.buckets[i];
            entry.tree = get_short(bytes.data() + offset);
            entry.elements = static_cast<uint8_t>(bytes[offset + 2]);
            if (!entry.tree || !entry.elements) return map_page_status::invalid;
            offset += 3;
        }
        if (offset != bytes.size()) return map_page_status::invalid;
    } else if (bytes[3] == 3) {
        if (bytes.size() != compact_group_bytes ||
            get_short(bytes.data() + 4) != tag ||
            get_short(bytes.data() + 6) != root.tree)
            return map_page_status::invalid;
        for (size_t i = 0; i < 16; ++i) {
            group.buckets[i].tree = get_short(bytes.data() + 8 + i * 4);
            group.buckets[i].elements = get_short(bytes.data() + 10 + i * 4);
        }
    } else {
        if (bytes.size() != group_bytes || get(bytes.data() + 8) != root.tree ||
            get_short(bytes.data() + 4) != tag || bytes[6] || bytes[7])
            return map_page_status::invalid;
        for (size_t i = 0; i < 16; ++i) {
            group.buckets[i].tree = get(bytes.data() + 16 + i * 16);
            group.buckets[i].elements = get(bytes.data() + 24 + i * 16);
        }
    }
    if (!valid(root, tag, group)) return map_page_status::invalid;
    out = group;
    return map_page_status::ok;
}
} // namespace pkv
