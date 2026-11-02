#include "string_store.h"
#include <algorithm>

namespace pkv {

bool string_store::getbit(uint64_t offset, long long& bit) {
    bit = 0;
    if (error_ != status::ok) return false;
    if (offset >= string_page_codec::write_bytes * 8)
        return fail(status::invalid_argument);
    const auto byte = offset / 8;
    if (!exists_ || byte >= root_.length) return true;
    std::string bytes;
    unsigned char value;
    if (!root_.pages) value = root_.inline_value[static_cast<size_t>(byte)];
    else {
        if (!read_page(byte / string_page_codec::page_bytes, bytes))
            return false;
        value = bytes[static_cast<size_t>(
            byte % string_page_codec::page_bytes)];
    }
    bit = (value >> (7 - offset % 8)) & 1;
    return true;
}

bool string_store::setbit(uint64_t offset, bool bit, long long& previous) {
    previous = 0;
    if (error_ != status::ok) return false;
    if (offset >= string_page_codec::write_bytes * 8)
        return fail(status::invalid_argument);
    const auto byte = offset / 8;
    const auto mask = static_cast<unsigned char>(1U << (7 - offset % 8));
    if (!exists_ || byte >= root_.length)
        return write(byte, std::string(1, static_cast<char>(bit ? mask : 0)));
    std::string bytes;
    const auto page = byte / string_page_codec::page_bytes;
    const auto position = static_cast<size_t>(root_.pages ?
        byte % string_page_codec::page_bytes : byte);
    if (root_.pages && !read_page(page, bytes)) return false;
    auto& target = root_.pages ? bytes : root_.inline_value;
    const auto value = static_cast<unsigned char>(target[position]);
    previous = (value & mask) ? 1 : 0;
    if (previous == static_cast<long long>(bit)) return true;
    target[position] = static_cast<char>(bit ? value | mask : value & ~mask);
    if (root_.pages && !write_page(page, target)) return false;
    dirty_ = true;
    return true;
}

bool string_store::scan_bits(uint64_t first, uint64_t last, int bit,
    long long& out) {
    out = bit < 0 ? 0 : -1;
    const auto last_byte = last / 8;
    for (auto offset = first / 8; offset <= last_byte;) {
        const auto page_end = (offset / string_page_codec::page_bytes + 1) *
            string_page_codec::page_bytes - 1;
        const auto end = std::min(last_byte, page_end);
        std::string bytes;
        if (!range(static_cast<long long>(offset),
            static_cast<long long>(end), bytes)) return false;
        unsigned first_mask = offset == first / 8 ? 255U >> (first % 8) : 255;
        unsigned last_mask = end == last_byte ? 255U << (7 - last % 8) : 255;
        if (bit == 0) {
            bytes.front() = static_cast<char>(
                static_cast<unsigned char>(bytes.front()) | ~first_mask);
            bytes.back() = static_cast<char>(
                static_cast<unsigned char>(bytes.back()) | ~last_mask);
        } else {
            bytes.front() = static_cast<char>(
                static_cast<unsigned char>(bytes.front()) & first_mask);
            bytes.back() = static_cast<char>(
                static_cast<unsigned char>(bytes.back()) & last_mask);
        }
        if (bit < 0) out += static_cast<long long>(
            bitmap_popcount(bytes.data(), bytes.size()));
        else {
            const auto found = bitmap_position(bytes.data(), bytes.size(), bit);
            if (found >= 0) {
                out = static_cast<long long>(offset * 8) + found;
                return true;
            }
        }
        offset = end + 1;
    }
    return true;
}

bool string_store::bitcount(const bitmap_range& range, long long& count) {
    count = 0;
    if (error_ != status::ok) return false;
    uint64_t first, last;
    if (!bitmap_bounds(root_.length, range, true, first, last)) return true;
    return scan_bits(first, last, -1, count);
}

bool string_store::bitpos(bool bit, const bitmap_range& range,
    long long& position) {
    position = -1;
    if (error_ != status::ok) return false;
    if (!exists_) { position = bit ? -1 : 0; return true; }
    uint64_t first, last;
    if (!bitmap_bounds(root_.length, range, false, first, last)) return true;
    if (!scan_bits(first, last, bit ? 1 : 0, position)) return false;
    if (position < 0 && !bit && !range.end_given)
        position = static_cast<long long>(root_.length * 8);
    return true;
}

} // namespace pkv
