#include "list_page.h"
#include <cassert>
#include <cstring>
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

void write32(char*& out, uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8)
        *out++ = static_cast<char>(value >> shift);
}

void write64(char*& out, uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8)
        *out++ = static_cast<char>(value >> shift);
}

uint32_t get32(const std::string& bytes, size_t offset) {
#if defined(__GNUC__) && defined(__BYTE_ORDER__) && \
    __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    uint32_t value;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return __builtin_bswap32(value);
#elif defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    uint32_t value;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
#else
    uint32_t value = 0;
    for (size_t i = 0; i < 4; ++i) {
        value = (value << 8) | static_cast<unsigned char>(bytes[offset + i]);
    }
    return value;
#endif
}

uint64_t get64(const std::string& bytes, size_t offset) {
#if defined(__GNUC__) && defined(__BYTE_ORDER__) && \
    __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    uint64_t value;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return __builtin_bswap64(value);
#elif defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    uint64_t value;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
#else
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) {
        value = (value << 8) | static_cast<unsigned char>(bytes[offset + i]);
    }
    return value;
#endif
}

list_page_status signature(const std::string& bytes, const char* prefix,
    unsigned version = 1) {
    if (bytes.size() < 4 || std::memcmp(bytes.data(), prefix, 3) != 0) {
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

template <bool bounded = false>
bool validate_page_bytes(const std::string& bytes,
    uint64_t member_limit = std::numeric_limits<uint64_t>::max(),
    size_t* last_fragment = nullptr) {
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
        if ((bounded && total > member_limit) ||
            get32(bytes, cursor + 20) != 0 ||
            size > bytes.size() - cursor - header) {
            return false;
        }
        // Complete members need no continuation checks. The previous fragment
        // must also have finished; this includes the first fragment's state.
        if ((total != size || offset != 0 || previous_end != previous_total) &&
            (!valid_fragment(total, offset, size) ||
            !valid_sequence(total, offset, previous_total, previous_end,
                i == 0) || (i == 0 && offset && get64(bytes, 16) == 0))) {
            return false;
        }
        previous_total = total;
        previous_end = offset + size;
        if (last_fragment) *last_fragment = cursor;
        cursor += header + size;
    }
    return cursor == bytes.size() && (previous_end == previous_total ||
        get64(bytes, 24) != 0);
}

} // namespace

list_page_status list_page_codec::encode_root(const list_root& root,
    std::string& out, bool* preserves_deadline) {
    if (!valid_root(root)) {
        return list_page_status::invalid;
    }
    try {
        char encoded[extended_root_bytes];
        std::memcpy(encoded, root.last_id ? "PLR\2" : "PLR\1", 4);
        char* cursor = encoded + 4;
        write32(cursor, 0);
        // Unsigned conversion is defined modulo 2^64, including persistent -1.
        write64(cursor, static_cast<uint64_t>(root.expires_at_ms));
        const auto deadline_end = cursor - encoded;
        write64(cursor, root.elements);
        write64(cursor, root.head);
        write64(cursor, root.tail);
        if (root.last_id) write64(cursor, root.last_id);
        // The native edit uses the existing extended root under its key lock.
        const bool same_deadline = preserves_deadline && root.last_id &&
            out.size() == extended_root_bytes &&
            !std::memcmp(out.data(), encoded, static_cast<size_t>(deadline_end));
        out.assign(encoded, static_cast<size_t>(cursor - encoded));
        if (preserves_deadline) *preserves_deadline = same_deadline;
        return list_page_status::ok;
    } catch (const std::bad_alloc&) {
        return list_page_status::out_of_memory;
    }
}

void list_page_codec::update_root_elements(std::string& bytes, uint64_t elements) {
    assert(bytes.size() == extended_root_bytes);
    auto* cursor = &bytes[0] + 16;
    write64(cursor, elements);
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
        std::string encoded(size, '\0');
        char* cursor = &encoded[0];
        std::memcpy(cursor, "PLP\1", 4); cursor += 4;
        write32(cursor, static_cast<uint32_t>(page.fragments.size()));
        write64(cursor, page.id);
        write64(cursor, page.previous);
        write64(cursor, page.next);
        for (const auto& item : page.fragments) {
            write64(cursor, item.total_bytes);
            write64(cursor, item.offset);
            write32(cursor, static_cast<uint32_t>(item.bytes.size()));
            cursor += 4; // Reserved bytes are already zero.
            if (!item.bytes.empty())
                std::memcpy(cursor, item.bytes.data(), item.bytes.size());
            cursor += item.bytes.size();
        }
        out.swap(encoded);
        return list_page_status::ok;
    } catch (const std::bad_alloc&) {
        return list_page_status::out_of_memory;
    }
}

list_page_status list_page_codec::push_end(const std::string& bytes,
    list_values_view values, bool left,
    uint64_t member_limit, list_page_update& out, size_t fill_target,
    uint64_t neighbor) {
    if (fill_target < page_header_bytes + fragment_header_bytes ||
        fill_target > max_page_bytes) return list_page_status::invalid;
    if (is_compact_page(bytes))
        return push_compact(bytes, values, left, member_limit, out,
            fill_target, neighbor);
    const auto status = signature(bytes, "PLP");
    if (status != list_page_status::ok) return status;
    if (!validate_page_bytes<true>(bytes, member_limit))
        return list_page_status::invalid;
    list_page_update update;
    update.id = get64(bytes, 8);
    update.previous = get64(bytes, 16); update.next = get64(bytes, 24);
    if (left ? update.previous != 0 : update.next != 0)
        return list_page_status::invalid;
    size_t size = bytes.size();
    const auto limit = size > fill_target ? max_page_bytes : fill_target;
    bool fits = !values.empty();
    for (const auto& value : values) {
        if (value.size() > member_limit) return list_page_status::invalid;
        if (fragment_header_bytes > limit - size ||
            value.size() > limit - size - fragment_header_bytes)
            fits = false;
        if (fits) size += fragment_header_bytes + value.size();
    }
    if (!fits) { out = std::move(update); return list_page_status::ok; }
    try {
        std::string encoded;
        encoded.reserve(size);
        encoded.append(bytes.data(), page_header_bytes);
        char* cursor = &encoded[0] + 4;
        write32(cursor, get32(bytes, 4) + static_cast<uint32_t>(values.size()));
        const auto previous = bytes.size() - page_header_bytes;
        if (!left) encoded.append(bytes, page_header_bytes, previous);
        for (size_t i = 0; i < values.size(); ++i) {
            const auto& value = values[left ? values.size() - i - 1 : i];
            char header[fragment_header_bytes] = {};
            cursor = header;
            write64(cursor, value.size()); write64(cursor, 0);
            write32(cursor, static_cast<uint32_t>(value.size()));
            encoded.append(header, sizeof(header));
            encoded.append(value);
        }
        if (left) encoded.append(bytes, page_header_bytes, previous);
        update.bytes.swap(encoded); update.applied = true;
        out = std::move(update);
        return list_page_status::ok;
    } catch (const std::bad_alloc&) {
        return list_page_status::out_of_memory;
    }
}

list_page_status list_page_codec::apply_fragment_pop_right(
    std::string& bytes, const list_page_edit& plan) {
    const size_t removed = plan.right_stride;
    if (!plan.id || plan.left || plan.expected_count < 2 ||
        removed < fragment_header_bytes || bytes.size() != plan.expected_bytes ||
        bytes.size() > max_page_bytes ||
        bytes.size() < page_header_bytes + fragment_header_bytes + removed ||
        signature(bytes, "PLP") != list_page_status::ok ||
        get64(bytes, 8) != plan.id || get64(bytes, 24) ||
        get32(bytes, 4) != plan.expected_count)
        return list_page_status::invalid;
    const auto last = bytes.size() - removed;
    const auto size = removed - fragment_header_bytes;
    if (get64(bytes, last) != size || get64(bytes, last + 8) ||
        get32(bytes, last + 16) != size || get32(bytes, last + 20))
        return list_page_status::invalid;
    return truncate_right(bytes, last, plan.expected_count - 1);
}

list_page_status list_page_codec::pop_end(const std::string& bytes,
    bool left, uint64_t member_limit, list_page_update& out, list_page_edit* edit) {
    if (is_compact_page(bytes))
        return pop_compact(bytes, left, member_limit, out);
    const auto status = signature(bytes, "PLP");
    if (status != list_page_status::ok) return status;
    size_t last = page_header_bytes;
    if (!validate_page_bytes<true>(bytes, member_limit, &last))
        return list_page_status::invalid;
    list_page_update update;
    update.id = get64(bytes, 8);
    update.previous = get64(bytes, 16); update.next = get64(bytes, 24);
    if (left ? update.previous != 0 : update.next != 0)
        return list_page_status::invalid;
    const size_t first = left ? page_header_bytes : last;
    const auto size = get32(bytes, first + 16);
    const auto count = get32(bytes, 4);
    if (count == 1 || get64(bytes, first + 8) != 0 ||
        get64(bytes, first) != size) {
        out = std::move(update);
        return list_page_status::ok;
    }
    if (!left && edit && !edit->id) {
        try {
            out.value.assign(bytes.data() + first + fragment_header_bytes, size);
        } catch (const std::bad_alloc&) { return list_page_status::out_of_memory; }
        update.value.swap(out.value);
        update.applied = true;
        edit->id = update.id;
        edit->expected_bytes = static_cast<uint32_t>(bytes.size());
        edit->expected_count = static_cast<uint16_t>(count);
        edit->length_bytes = fragment_header_bytes;
        edit->right_stride = static_cast<uint16_t>(bytes.size() - first);
        out = std::move(update);
        return list_page_status::ok;
    }
    try {
        const size_t removed = fragment_header_bytes + size;
        const size_t size_after = bytes.size() - removed;
        std::string encoded;
        encoded.reserve(size_after);
        encoded.append(bytes.data(), page_header_bytes);
        const auto start = page_header_bytes + (left ? removed : 0);
        encoded.append(bytes, start, size_after - page_header_bytes);
        char* cursor = &encoded[0] + 4;
        write32(cursor, count - 1);
        update.value.assign(bytes.data() + first + fragment_header_bytes,
            size);
        update.bytes.swap(encoded); update.applied = true;
        out = std::move(update);
        return list_page_status::ok;
    } catch (const std::bad_alloc&) {
        return list_page_status::out_of_memory;
    }
}

list_page_status list_page_codec::decode_page(const std::string& bytes,
    list_page& out) {
    if (is_compact_page(bytes))
        return decode_compact(bytes, out);
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
