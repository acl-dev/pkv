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
    unsigned char value;
    if (!root_.pages) value = root_.inline_value[static_cast<size_t>(byte)];
    else {
        const auto index = byte / string_page_codec::page_bytes;
        struct read_byte {
            size_t page_size, offset;
            unsigned char value = 0;
            bool valid = false;
            read_byte(size_t size, size_t pos) : page_size(size), offset(pos) {}
        } selected(string_page_codec::page_size(root_, index),
            static_cast<size_t>(byte % string_page_codec::page_bytes));
        const auto inspect = [](void* raw, const std::string& bytes) {
            auto& selected = *static_cast<read_byte*>(raw);
            if (bytes.size() != selected.page_size) return;
            selected.value = static_cast<unsigned char>(bytes[selected.offset]);
            selected.valid = true;
        };
        std::string scratch;
        const auto status = transaction_.inspect_bound(entry_, index + 1,
            scratch, inspect, &selected);
        if (status != db::lookup_status::found || !selected.valid)
            return fail(status == db::lookup_status::error ?
                string_store::status::storage : string_store::status::invalid);
        value = selected.value;
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
    const auto first_byte = first / 8;
    const auto last_byte = last / 8;
    std::string scratch;
    for (auto offset = first_byte; offset <= last_byte;) {
        const auto page_end = (offset / string_page_codec::page_bytes + 1) *
            string_page_codec::page_bytes - 1;
        const auto end = std::min(last_byte, page_end);
        const auto size = static_cast<size_t>(end - offset + 1);
        unsigned first_mask = offset == first_byte ? 255U >> (first % 8) : 255;
        unsigned last_mask = end == last_byte ? (255U << (7 - last % 8)) & 255 : 255;
        if (size == 1) { first_mask &= last_mask; last_mask = 255; }

        auto scan = [&](const char* bytes) {
            auto consume = [&](const char* data, size_t count, uint64_t base) {
                if (bit < 0) {
                    out += static_cast<long long>(bitmap_popcount(data, count));
                    return false;
                }
                const auto found = bitmap_position(data, count, bit);
                if (found < 0) return false;
                out = static_cast<long long>(base * 8) + found;
                return true;
            };
            auto masked = [&](char byte, unsigned mask, uint64_t position) {
                const auto value = static_cast<unsigned char>(byte);
                const auto adjusted = static_cast<char>(
                    bit == 0 ? value | ~mask : value & mask);
                return consume(&adjusted, 1, position);
            };
            size_t start = 0, stop = size;
            if (first_mask != 255) {
                if (masked(bytes[0], first_mask, offset)) return;
                ++start;
            }
            if (last_mask != 255) --stop;
            if (start < stop && consume(bytes + start, stop - start, offset + start))
                return;
            if (last_mask != 255) masked(bytes[stop], last_mask, end);
        };
        if (!root_.pages) {
            scan(root_.inline_value.data() + static_cast<size_t>(offset));
        } else {
            const auto index = offset / string_page_codec::page_bytes;
            struct scan_page {
                decltype(scan)& run;
                size_t length, offset;
                bool valid;
            } chunk{scan, string_page_codec::page_size(root_, index),
                static_cast<size_t>(offset % string_page_codec::page_bytes), false};
            const auto inspect = [](void* raw, const std::string& bytes) {
                auto& page = *static_cast<scan_page*>(raw);
                if (bytes.size() != page.length) return;
                page.run(bytes.data() + page.offset);
                page.valid = true;
            };
            const auto result = transaction_.inspect_bound(entry_, index + 1,
                scratch, inspect, &chunk);
            if (result != db::lookup_status::found || !chunk.valid)
                return fail(result == db::lookup_status::error ?
                    status::storage : status::invalid);
        }
        if (bit >= 0 && out >= 0) return true;
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
