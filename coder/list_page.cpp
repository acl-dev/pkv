#include "list_page.h"
#include <limits>
#include <new>
#include <utility>

namespace pkv {

const size_t list_page_codec::root_bytes;
const size_t list_page_codec::extended_root_bytes;
const size_t list_page_codec::page_header_bytes;
const size_t list_page_codec::fragment_header_bytes;
const size_t list_page_codec::max_page_bytes;
const size_t list_page_codec::max_fragment_bytes;

namespace {

void put32(std::string& out, uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<char>(value >> shift));
    }
}

void put64(std::string& out, uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<char>(value >> shift));
    }
}

uint32_t get32(const std::string& bytes, size_t offset) {
    uint32_t value = 0;
    for (size_t i = 0; i < 4; ++i) {
        value = (value << 8) | static_cast<unsigned char>(bytes[offset + i]);
    }
    return value;
}

uint64_t get64(const std::string& bytes, size_t offset) {
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) {
        value = (value << 8) | static_cast<unsigned char>(bytes[offset + i]);
    }
    return value;
}

list_page_status signature(const std::string& bytes, const char* prefix,
    unsigned version = 1) {
    if (bytes.size() < 4 || bytes.compare(0, 3, prefix, 3) != 0) {
        return list_page_status::invalid;
    }
    return bytes[3] >= 1 && static_cast<unsigned char>(bytes[3]) <= version ?
        list_page_status::ok
        : list_page_status::unsupported_version;
}

bool valid_root(const list_root& root) {
    if (root.expires_at_ms < -1 || root.elements >
        static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        return false;
    }
    if (root.last_id && (root.head > root.last_id || root.tail > root.last_id))
        {
        return false;
    }
    return root.elements ? root.head != 0 && root.tail != 0
        : root.head == 0 && root.tail == 0;
}

bool valid_links(uint64_t id, uint64_t previous, uint64_t next) {
    return id != 0 && previous != id && next != id &&
        (previous == 0 || previous != next);
}

bool valid_fragment(uint64_t total, uint64_t offset, size_t size) {
    if (offset > total || size > total - offset) {
        return false;
    }
    // A nonempty member cannot contain a zero-progress fragment.
    return total == 0 ? offset == 0 && size == 0 : size != 0;
}

// Within a page, unfinished members must continue contiguously. Only the
// first fragment may continue a member from the previous page, and only the
// last fragment may continue into the next page. Cross-page matching is a
// transaction/tree invariant, not something one page can establish alone.
bool valid_sequence(uint64_t total, uint64_t offset, uint64_t previous_total,
    uint64_t previous_end, bool first) {
    if (first) {
        return true;
    }
    return previous_end < previous_total ?
        total == previous_total && offset == previous_end : offset == 0;
}

bool page_size(const list_page& page, size_t& size) {
    size = list_page_codec::page_header_bytes;
    if (!valid_links(page.id, page.previous, page.next) ||
        page.fragments.empty()) {
        return false;
    }
    uint64_t previous_total = 0, previous_end = 0;
    bool first = true;
    for (const auto& item : page.fragments) {
        if (!valid_fragment(item.total_bytes, item.offset, item.bytes.size())
            || !valid_sequence(item.total_bytes, item.offset, previous_total,
                previous_end, first)) {
            return false;
        }
        const auto header = list_page_codec::fragment_header_bytes;
        if (header > list_page_codec::max_page_bytes - size ||
            item.bytes.size() > list_page_codec::max_page_bytes - size -
                header) {
            return false;
        }
        size += header + item.bytes.size();
        previous_total = item.total_bytes;
        previous_end = item.offset + item.bytes.size();
        first = false;
    }
    // Boundary links must exist for members continuing outside this page.
    return (page.fragments.front().offset == 0 || page.previous != 0) &&
        (previous_end == previous_total || page.next != 0);
}

bool validate_page_bytes(const std::string& bytes) {
    if (bytes.size() < list_page_codec::page_header_bytes ||
        bytes.size() > list_page_codec::max_page_bytes ||
        !valid_links(get64(bytes, 8), get64(bytes, 16), get64(bytes, 24))) {
        return false;
    }
    const auto count = get32(bytes, 4);
    const auto header = list_page_codec::fragment_header_bytes;
    if (!count || count > (bytes.size() -
        list_page_codec::page_header_bytes) / header) {
        return false;
    }
    uint64_t previous_total = 0, previous_end = 0;
    size_t cursor = list_page_codec::page_header_bytes;
    for (uint32_t i = 0; i < count; ++i) {
        if (header > bytes.size() - cursor) {
            return false;
        }
        const auto total = get64(bytes, cursor);
        const auto offset = get64(bytes, cursor + 8);
        const auto size = get32(bytes, cursor + 16);
        if (get32(bytes, cursor + 20) != 0 ||
            size > bytes.size() - cursor - header ||
            !valid_fragment(total, offset, size) ||
            !valid_sequence(total, offset, previous_total, previous_end,
                i == 0) || (i == 0 && offset && get64(bytes, 16) == 0)) {
            return false;
        }
        previous_total = total;
        previous_end = offset + size;
        cursor += header + size;
    }
    return cursor == bytes.size() && (previous_end == previous_total ||
        get64(bytes, 24) != 0);
}

} // namespace

list_page_status list_page_codec::encode_root(const list_root& root,
    std::string& out) {
    if (!valid_root(root)) {
        return list_page_status::invalid;
    }
    try {
        std::string encoded(root.last_id ? "PLR\2" : "PLR\1", 4);
        encoded.reserve(root.last_id ? extended_root_bytes : root_bytes);
        put32(encoded, 0);
        // Unsigned conversion is defined modulo 2^64, including persistent -1.
        put64(encoded, static_cast<uint64_t>(root.expires_at_ms));
        put64(encoded, root.elements);
        put64(encoded, root.head);
        put64(encoded, root.tail);
        if (root.last_id) put64(encoded, root.last_id);
        out.swap(encoded);
        return list_page_status::ok;
    } catch (const std::bad_alloc&) {
        return list_page_status::out_of_memory;
    }
}

list_page_status list_page_codec::decode_root(const std::string& bytes,
    list_root& out) {
    const auto status = signature(bytes, "PLR", 2);
    if (status != list_page_status::ok) {
        return status;
    }
    const bool extended = bytes[3] == 2;
    if (bytes.size() != (extended ? extended_root_bytes : root_bytes) ||
        get32(bytes, 4) != 0) {
        return list_page_status::invalid;
    }
    const auto expiry = get64(bytes, 8);
    const auto persistent = std::numeric_limits<uint64_t>::max();
    if (expiry != persistent && expiry >
        static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        return list_page_status::invalid;
    }
    list_root root;
    root.expires_at_ms = expiry == persistent ? -1 :
        static_cast<int64_t>(expiry);
    root.elements = get64(bytes, 16);
    root.head = get64(bytes, 24);
    root.tail = get64(bytes, 32);
    root.last_id = extended ? get64(bytes, 40) : 0;
    if ((extended && !root.last_id) || !valid_root(root)) {
        return list_page_status::invalid;
    }
    out = root;
    return list_page_status::ok;
}

list_page_status list_page_codec::encode_page(const list_page& page,
    std::string& out) {
    size_t size;
    if (!page_size(page, size)) {
        return list_page_status::invalid;
    }
    try {
        std::string encoded("PLP\1", 4);
        encoded.reserve(size);
        put32(encoded, static_cast<uint32_t>(page.fragments.size()));
        put64(encoded, page.id);
        put64(encoded, page.previous);
        put64(encoded, page.next);
        for (const auto& item : page.fragments) {
            put64(encoded, item.total_bytes);
            put64(encoded, item.offset);
            put32(encoded, static_cast<uint32_t>(item.bytes.size()));
            put32(encoded, 0);
            encoded.append(item.bytes);
        }
        out.swap(encoded);
        return list_page_status::ok;
    } catch (const std::bad_alloc&) {
        return list_page_status::out_of_memory;
    }
}

list_page_status list_page_codec::decode_page(const std::string& bytes,
    list_page& out) {
    const auto status = signature(bytes, "PLP");
    if (status != list_page_status::ok) {
        return status;
    }
    // Verify every length, count and offset before allocating any fragments.
    if (!validate_page_bytes(bytes)) {
        return list_page_status::invalid;
    }
    try {
        list_page page;
        page.id = get64(bytes, 8);
        page.previous = get64(bytes, 16);
        page.next = get64(bytes, 24);
        const auto count = get32(bytes, 4);
        page.fragments.reserve(count);
        size_t cursor = page_header_bytes;
        for (uint32_t i = 0; i < count; ++i) {
            list_fragment item;
            item.total_bytes = get64(bytes, cursor);
            item.offset = get64(bytes, cursor + 8);
            const auto size = get32(bytes, cursor + 16);
            cursor += fragment_header_bytes;
            item.bytes.assign(bytes, cursor, size);
            page.fragments.push_back(std::move(item));
            cursor += size;
        }
        out = std::move(page);
        return list_page_status::ok;
    } catch (const std::bad_alloc&) {
        return list_page_status::out_of_memory;
    }
}

} // namespace pkv
