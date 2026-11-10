#include "packed_hash.h"
#include <new>

namespace pkv { namespace packed_hash {
namespace {
constexpr unsigned deadline_flag = 0x80;
bool valid(const root& value) {
    const auto groups = value.groups();
    if (!groups || !value.elements || value.elements > max_fields ||
        value.expires_at_ms < -1) return false;
    unsigned total = 0;
    for (unsigned i = 0; i < max_groups; ++i) {
        if (value.sizes[i] > max_group_fields ||
            (i >= groups && value.sizes[i])) return false;
        total += value.sizes[i];
    }
    return total == value.elements;
}
}
bool matches(const std::string& bytes) noexcept {
    return bytes.compare(0, 4, "PMR\14", 4) == 0;
}
unsigned creation_bits(size_t fields) noexcept {
    if (fields <= compact_hash::max_fields || fields > max_fields) return 0;
    unsigned bits = min_bits;
    while (bits < max_bits && fields > (size_t{1} << bits) * 8) ++bits;
    return bits;
}
map_page_status encode_root(const root& value, std::string& out) {
    if (!valid(value)) return map_page_status::invalid;
    const size_t length = 7 + (value.expires_at_ms >= 0 ? 8 : 0) + value.groups();
    if (out.size() == length) {
        // The root is an owned transaction buffer. Fixed-size publication is
        // nonthrowing after validation; no borrowed metadata points into it.
        std::memcpy(&out[0], "PMR\14", 4);
        out[4] = char(value.bits | (value.expires_at_ms >= 0 ? deadline_flag : 0));
        out[5] = char(value.elements >> 8); out[6] = char(value.elements);
        size_t start = 7;
        if (value.expires_at_ms >= 0) {
            compact_zset::write_deadline(&out[7],value.expires_at_ms); start = 15;
        }
        for (unsigned i = 0; i < value.groups(); ++i) out[start + i] = char(value.sizes[i]);
        return map_page_status::ok;
    }
    try {
        std::string bytes("PMR\14", 4);
        bytes.reserve(7 + (value.expires_at_ms >= 0 ? 8 : 0) + value.groups());
        bytes.push_back(char(value.bits |
            (value.expires_at_ms >= 0 ? deadline_flag : 0)));
        bytes.push_back(char(value.elements >> 8));
        bytes.push_back(char(value.elements));
        if (value.expires_at_ms >= 0) {
            char deadline[8];
            compact_zset::write_deadline(deadline, value.expires_at_ms);
            bytes.append(deadline, sizeof(deadline));
        }
        for (unsigned i = 0; i < value.groups(); ++i)
            bytes.push_back(char(value.sizes[i]));
        out.swap(bytes);
        return map_page_status::ok;
    } catch (const std::bad_alloc&) {
        return map_page_status::out_of_memory;
    }
}
map_page_status decode_root(const std::string& bytes, root& out) noexcept {
    if (bytes.size() < 4 || bytes.compare(0, 3, "PMR", 3) != 0)
        return map_page_status::invalid;
    if (!matches(bytes)) return map_page_status::unsupported;
    if (bytes.size() < 7) return map_page_status::invalid;
    const auto flags = compact_hash::byte(bytes[4]);
    root value;
    value.bits = flags & 7;
    const auto groups = value.groups();
    const size_t start = 7 + (flags & deadline_flag ? 8 : 0);
    if ((flags & 0x78) || !groups || bytes.size() != start + groups)
        return map_page_status::invalid;
    value.elements = uint16_t((unsigned(compact_hash::byte(bytes[5])) << 8) |
        compact_hash::byte(bytes[6]));
    if (flags & deadline_flag) {
        uint64_t deadline = 0;
        for (unsigned i = 7; i < 15; ++i)
            deadline = (deadline << 8) | compact_hash::byte(bytes[i]);
        if (deadline > INT64_MAX) return map_page_status::invalid;
        value.expires_at_ms = int64_t(deadline);
    }
    for (unsigned i = 0; i < groups; ++i)
        value.sizes[i] = compact_hash::byte(bytes[start + i]);
    if (!valid(value)) return map_page_status::invalid;
    out = value;
    return map_page_status::ok;
}
} }
