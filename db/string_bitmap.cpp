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
            auto& task = *static_cast<read_byte*>(raw);
            if (bytes.size() != task.page_size) return;
            task.value = static_cast<unsigned char>(bytes[task.offset]);
            task.valid = true;
        };
        std::string scratch;
        const auto lookup = transaction_.inspect_bound(entry_, index + 1,
            scratch, inspect, &selected);
        if (lookup != db::lookup_status::found || !selected.valid)
            return fail(lookup == db::lookup_status::error ?
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
    unsigned char value;
    if (root_.pages) {
        struct bit_update {
            size_t size, position;
            unsigned char mask, wanted;
            std::string& replacement;
            unsigned char value;
            bool valid;
        } update{string_page_codec::page_size(root_, page), position,
            mask, static_cast<unsigned char>(bit ? mask : 0), bytes, 0, false};
        const auto inspect = [](void* raw, const std::string& data) {
            auto& request = *static_cast<bit_update*>(raw);
            if (data.size() != request.size) return;
            request.value = static_cast<unsigned char>(data[request.position]);
            // A fallback backend may have already copied into replacement.
            if ((request.value & request.mask) != request.wanted &&
                &data != &request.replacement)
                request.replacement = data;
            request.valid = true;
        };
        const auto found = transaction_.inspect_bound(entry_, page + 1,
            bytes, inspect, &update);
        if (found != db::lookup_status::found || !update.valid)
            return fail(found == db::lookup_status::error ? status::storage :
                status::invalid);
        value = update.value;
    } else value = static_cast<unsigned char>(root_.inline_value[position]);
    previous = (value & mask) ? 1 : 0;
    if (previous == static_cast<long long>(bit)) return true;
    auto& target = root_.pages ? bytes : root_.inline_value;
    target[position] = static_cast<char>(bit ? value | mask : value & ~mask);
    if (root_.pages && !write_page(page, std::move(bytes))) return false;
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
        const auto scan = [&](const char* bytes) {
            bitmap_scan_chunk(bytes, offset, size, first, last, bit, out);
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
