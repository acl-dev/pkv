#include "list_page.h"
#include "validated_page_cache.h"
#include "db/node_write_workspace.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <utility>
#if defined(__APPLE__) && !defined(HAS_JEMALLOC)
#include <malloc/malloc.h>
#endif

namespace pkv {
namespace {
using codec = list_page_codec;
using status = list_page_status;
const size_t packed_header = 4;


uint32_t read32(const std::string& bytes, size_t at) {
#if defined(__GNUC__) && defined(__BYTE_ORDER__) && \
    __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    uint32_t value;
    std::memcpy(&value, bytes.data() + at, sizeof(value));
    return __builtin_bswap32(value);
#elif defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    uint32_t value;
    std::memcpy(&value, bytes.data() + at, sizeof(value));
    return value;
#else
    uint32_t value = 0;
    for (size_t i = 0; i < 4; ++i)
        value = (value << 8) | static_cast<unsigned char>(bytes[at + i]);
    return value;
#endif
}
uint64_t read64(const std::string& bytes, size_t at) {
#if defined(__GNUC__) && defined(__BYTE_ORDER__) && \
    __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    uint64_t value;
    std::memcpy(&value, bytes.data() + at, sizeof(value));
    return __builtin_bswap64(value);
#elif defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    uint64_t value;
    std::memcpy(&value, bytes.data() + at, sizeof(value));
    return value;
#else
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i)
        value = (value << 8) | static_cast<unsigned char>(bytes[at + i]);
    return value;
#endif
}
void write32(char*& out, uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8)
        *out++ = static_cast<char>(value >> shift);
}
void write64(char*& out, uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8)
        *out++ = static_cast<char>(value >> shift);
}
uint32_t read_length(const std::string& bytes, size_t at, size_t header) {
    return header == 1 ? static_cast<unsigned char>(bytes[at]) : read32(bytes, at);
}
void write_length(char*& out, uint32_t value, size_t header) {
    if (header == 1) *out++ = static_cast<char>(value);
    else write32(out, value);
}
bool links(uint64_t id, uint64_t previous, uint64_t next) {
    return id && previous != id && next != id &&
        (!previous || previous != next);
}
struct page_view {
    uint64_t id, previous, next;
    uint32_t count;
    size_t expanded, last, header;
};

bool inspect_uniform(const std::string& bytes, uint64_t limit,
    page_view& view, size_t* verified_stride) {
    // Validate independent length fields in groups. The encoded body size
    // bounds every access before the first load, including zero-byte values.
    const auto body = bytes.size() - codec::page_header_bytes;
    if (body % view.count) return false;
    const auto stride = body / view.count;
    if (stride < view.header || stride - view.header > limit)
        return false;
    const auto size = static_cast<uint32_t>(stride - view.header);
    size_t cursor = codec::page_header_bytes;
    uint32_t remaining = view.count;
    for (; remaining >= 4; remaining -= 4, cursor += 4 * stride) {
        const auto mismatches = (read_length(bytes, cursor, view.header) ^ size) |
            (read_length(bytes, cursor + stride, view.header) ^ size) |
            (read_length(bytes, cursor + 2 * stride, view.header) ^ size) |
            (read_length(bytes, cursor + 3 * stride, view.header) ^ size);
        if (mismatches) return false;
    }
    for (; remaining; --remaining, cursor += stride)
        if (read_length(bytes, cursor, view.header) != size) return false;
    view.last = cursor - stride;
    if (verified_stride) *verified_stride = stride;
    return true;
}

bool inspect_members(const std::string& bytes, uint64_t limit, uint32_t count,
    size_t& cursor, size_t& last, size_t header) {
    for (uint32_t i = 0; i < count; ++i) {
        if (header > bytes.size() - cursor) return false;
        const auto size = read_length(bytes, cursor, header);
        if (size > limit || size > bytes.size() - cursor - header)
            return false;
        last = cursor;
        cursor += header + size;
    }
    return true;
}

struct batch_offsets {
    uint32_t prefix;
    size_t start = codec::page_header_bytes;
    size_t stride = 0;
    explicit batch_offsets(uint32_t count) : prefix(count) {}
};

const uint32_t checkpoint_width = 16;
struct right_page_view {
    page_view page;
    uint16_t offsets[codec::max_page_bytes /
        codec::fragment_header_bytes / checkpoint_width + 1];
};
using right_validation_cache = detail::validated_page_cache<right_page_view, 8192>;

bool admit_right_cache(const std::string& bytes) {
    // The address only gates admission. Cache hits still compare all bytes.
    static thread_local uintptr_t last_page = 0;
    const auto address = reinterpret_cast<uintptr_t>(&bytes);
    const bool repeated = last_page == address;
    last_page = address;
    return repeated && bytes.size() >= codec::page_header_bytes &&
        bytes.size() <= right_validation_cache::max_bytes && read32(bytes, 4) >= 32;
}

size_t member_offset(const std::string& bytes, const right_page_view& view,
    uint32_t member) {
    const auto checkpoint = member / checkpoint_width;
    size_t offset = view.offsets[checkpoint];
    for (auto i = checkpoint * checkpoint_width; i < member; ++i)
        offset += view.page.header + read_length(bytes, offset, view.page.header);
    return offset;
}

void remember_right_prefix(right_validation_cache& cache,
    const std::string& bytes, const std::string& encoded, uint64_t limit,
    right_page_view& indexed, uint32_t count) {
    auto& view = indexed.page;
    view.count -= count;
    view.expanded -= bytes.size() - encoded.size() + (codec::fragment_header_bytes - view.header) * count;
    view.last = member_offset(bytes, indexed, view.count - 1);
    cache.remember(encoded, limit, indexed);
}

// Every entry is a complete member, including length zero. The expanded
// bound keeps existing fragmentation/packing algorithms valid after decode.
bool inspect(const std::string& bytes, uint64_t limit, page_view& view,
    batch_offsets* batch = nullptr, uint16_t* checkpoints = nullptr) {
    if (bytes.size() < codec::page_header_bytes ||
        bytes.size() > codec::max_page_bytes ||
        !codec::is_compact_page(bytes)) return false;
    view.id = read64(bytes, 8); view.previous = read64(bytes, 16);
    view.next = read64(bytes, 24); view.count = read32(bytes, 4);
    view.header = bytes[3] == 3 ? 1 : packed_header;
    const auto saved_header = codec::fragment_header_bytes - view.header;
    const auto prefix = batch ? batch->prefix : 0;
    if (!links(view.id, view.previous, view.next) || !view.count ||
        view.count > (codec::max_page_bytes - codec::page_header_bytes) /
            codec::fragment_header_bytes ||
        saved_header * view.count > codec::max_page_bytes - bytes.size() ||
        prefix >= view.count)
        return false;
    view.expanded = bytes.size() + saved_header * view.count;
    if (view.count >= 16 &&
        inspect_uniform(bytes, limit, view, batch ? &batch->stride : nullptr)) {
        if (batch) batch->start = codec::page_header_bytes + prefix * batch->stride;
        return true;
    }
    size_t cursor = codec::page_header_bytes;
    if (checkpoints) {
        for (uint32_t i = 0; i < view.count; i += checkpoint_width) {
            checkpoints[i / checkpoint_width] = static_cast<uint16_t>(cursor);
            const auto count = std::min(checkpoint_width, view.count - i);
            if (!inspect_members(bytes, limit, count, cursor, view.last, view.header))
                return false;
        }
        return cursor == bytes.size();
    }
    if (batch) {
        if (!inspect_members(bytes, limit, prefix, cursor, view.last, view.header))
            return false;
        batch->start = cursor;
    }
    return inspect_members(bytes, limit, view.count - prefix, cursor,
        view.last, view.header) && cursor == bytes.size();
}
void identify(const page_view& view, list_page_update& update) {
    update.id = view.id; update.previous = view.previous;
    update.next = view.next;
}

void write_page_header(char*& cursor, const list_page& page, uint32_t count,
    size_t header) {
    std::memcpy(cursor, header == 1 ? "PLP\3" : "PLP\2", 4); cursor += 4;
    write32(cursor, count);
    write64(cursor, page.id); write64(cursor, page.previous);
    write64(cursor, page.next);
}
} // namespace

list_page_status list_page_codec::encode_packed_page(const list_page& page,
    std::string& out) {
    for (const auto& item : page.fragments)
        if (item.offset || item.total_bytes != item.bytes.size())
            return encode_page(page, out);
    if (page.fragments.empty() || !links(page.id, page.previous, page.next))
        return status::invalid;
    size_t expanded = page_header_bytes, header = 1;
    for (const auto& item : page.fragments) {
        if (fragment_header_bytes > max_page_bytes - expanded ||
            item.bytes.size() > max_page_bytes - expanded -
                fragment_header_bytes) return status::invalid;
        expanded += fragment_header_bytes + item.bytes.size();
        if (item.bytes.size() > UINT8_MAX) header = packed_header;
    }
    try {
        const auto size = expanded -
            (fragment_header_bytes - header) * page.fragments.size();
        std::string encoded(size, '\0');
        char* cursor = &encoded[0];
        write_page_header(cursor, page,
            static_cast<uint32_t>(page.fragments.size()), header);
        for (const auto& item : page.fragments) {
            write_length(cursor, static_cast<uint32_t>(item.bytes.size()), header);
            if (!item.bytes.empty())
                std::memcpy(cursor, item.bytes.data(), item.bytes.size());
            cursor += item.bytes.size();
        }
        out.swap(encoded); return status::ok;
    } catch (const std::bad_alloc&) { return status::out_of_memory; }
}

list_page_status list_page_codec::encode_single_member(const list_page& page,
    const std::string& value, std::string& out) {
    if (!page.fragments.empty() || !links(page.id, page.previous, page.next) ||
        value.size() > max_page_bytes - page_header_bytes - fragment_header_bytes)
        return status::invalid;
    try {
        char header[page_header_bytes + packed_header];
        char* cursor = header;
        const auto width = value.size() <= UINT8_MAX ? 1 : packed_header;
        write_page_header(cursor, page, 1, width);
        write_length(cursor, static_cast<uint32_t>(value.size()), width);
        const auto size = page_header_bytes + width + value.size();
        detail::node_write_workspace workspace(
            detail::node_write_workspace::fits(size));
        auto& encoded = workspace.buffer();
        encoded.reserve(size);
        encoded.assign(header, page_header_bytes + width);
        encoded.append(value);
        out.swap(encoded);
        return status::ok;
    } catch (const std::bad_alloc&) { return status::out_of_memory; }
}

list_page_status list_page_codec::index_compact(const std::string& bytes,
    uint64_t index, bool reverse, uint64_t member_limit,
    uint32_t& count, list_page_update& out, const std::string* replacement) {
    count = 0;
    out.applied = false;
    if (!codec::is_compact_page(bytes)) return status::ok;
    if (bytes.size() < page_header_bytes) return status::invalid;
    const auto members = read32(bytes, 4);
    const bool selected = index < members;
    const auto offset = selected ? static_cast<uint32_t>(index) : 0;
    batch_offsets location(selected && reverse ? members - offset - 1 : offset);
    page_view page;
    if (!inspect(bytes, member_limit, page, selected ? &location : nullptr))
        return status::invalid;
    identify(page, out);
    if (selected) {
        try {
            const auto size = read_length(bytes, location.start, page.header);
            const auto start = location.start + page.header;
            if (!replacement) out.value.assign(bytes, start, size);
            else if ((page.header != 1 || replacement->size() <= UINT8_MAX) &&
                replacement->size() <= member_limit &&
                replacement->size() <= max_page_bytes - page.expanded + size) {
                out.bytes = bytes;
                out.bytes.replace(start, size, *replacement);
                if (replacement->size() != size) {
                    char* header = &out.bytes[location.start];
                    write_length(header, static_cast<uint32_t>(replacement->size()), page.header);
                }
                out.applied = true;
            }
        } catch (const std::bad_alloc&) { return status::out_of_memory; }
    }
    count = page.count;
    return status::ok;
}

list_page_status list_page_codec::range_compact(const std::string& bytes,
    uint64_t skip, uint64_t take, bool reverse, uint64_t member_limit,
    uint32_t& count, list_page_update& out, std::vector<std::string>& values) {
    count = 0;
    if (!codec::is_compact_page(bytes)) return status::ok;
    if (bytes.size() < page_header_bytes) return status::invalid;
    const auto members = read32(bytes, 4);
    const auto selected = skip < members ? std::min(take, members - skip) : 0;
    const auto first = selected ? (reverse ? members - skip - selected : skip) : 0;
    batch_offsets location(static_cast<uint32_t>(first));
    page_view page;
    if (!inspect(bytes, member_limit, page, selected ? &location : nullptr))
        return status::invalid;
    identify(page, out);
    try {
        const auto start = values.size();
        size_t cursor = location.start;
        for (uint64_t i = 0; i < selected; ++i) {
            const auto size = read_length(bytes, cursor, page.header);
            cursor += page.header;
            values.emplace_back(bytes, cursor, size);
            cursor += size;
        }
        if (reverse) std::reverse(values.begin() + start, values.end());
    } catch (const std::bad_alloc&) { return status::out_of_memory; }
    count = page.count;
    return status::ok;
}

list_page_status list_page_codec::decode_compact(const std::string& bytes,
    list_page& out) {
    page_view view;
    if (!inspect(bytes, std::numeric_limits<uint64_t>::max(), view))
        return status::invalid;
    try {
        list_page page;
        page.id = view.id; page.previous = view.previous; page.next = view.next;
        page.fragments.reserve(view.count);
        size_t cursor = page_header_bytes;
        for (uint32_t i = 0; i < view.count; ++i) {
            const auto size = read_length(bytes, cursor, view.header);
            cursor += view.header;
            list_fragment item;
            item.total_bytes = size; item.bytes.assign(bytes, cursor, size);
            page.fragments.push_back(std::move(item)); cursor += size;
        }
        out = std::move(page); return status::ok;
    } catch (const std::bad_alloc&) { return status::out_of_memory; }
}

list_page_status list_page_codec::plan_append(const std::string& bytes,
    const std::string& value, uint64_t member_limit, size_t fill_target,
    list_page_update& update, list_page_edit& plan, bool cached_only) {
    return plan_push(bytes, value, false, member_limit, fill_target,
        update, plan, cached_only);
}

list_page_status list_page_codec::plan_push(const std::string& bytes,
    const std::string& value, bool left, uint64_t member_limit,
    size_t fill_target, list_page_update& update, list_page_edit& plan,
    bool cached_only) {
    static_assert(max_page_bytes <= std::numeric_limits<uint16_t>::max(),
        "bounded compact fragment count must fit the push descriptor");
    if (plan.id || value.size() > max_fragment_bytes || !is_compact_page(bytes) ||
        (bytes[3] == 3 && value.size() > UINT8_MAX)) return status::ok;
    if (fill_target < page_header_bytes + fragment_header_bytes ||
        fill_target > max_page_bytes) return status::invalid;
    using validation_cache = detail::validated_page_cache<page_view>;
    const bool eligible = bytes.size() >= page_header_bytes &&
        bytes.size() <= validation_cache::max_bytes && read32(bytes, 4) >= 8;
    validation_cache cache(eligible);
    page_view view;
    const bool hit = cache.lookup(bytes, member_limit, view);
    // Random key/page switches use the complete writer without a second scan
    // or copying a cold snapshot solely to prepare an append descriptor.
    if (cached_only && !hit) return status::ok;
    if (!hit && !inspect(bytes, member_limit, view)) return status::invalid;
    if ((left ? view.previous : view.next) || value.size() > member_limit)
        return status::invalid;
    const auto limit = view.expanded > fill_target ? max_page_bytes : fill_target;
    if (fragment_header_bytes > limit - view.expanded ||
        value.size() > limit - view.expanded - fragment_header_bytes)
        return status::ok;
    try {
        list_page_edit next;
        next.expected_bytes = static_cast<uint32_t>(bytes.size());
        next.expected_count = static_cast<uint16_t>(view.count);
        next.left = left;
        next.length_bytes = static_cast<uint8_t>(view.header);
        char header[packed_header];
        char* length = header;
        write_length(length, static_cast<uint32_t>(value.size()), view.header);
        next.suffix.assign(header, view.header);
        next.suffix.append(value);
        next.id = view.id;
        identify(view, update);
        if (view.count + 1 >= 8 &&
            bytes.size() + next.suffix.size() <= validation_cache::max_bytes) {
            cache.activate();
            if (!hit) cache.remember(bytes, member_limit, view);
            auto after = view;
            ++after.count;
            after.last = left ? view.last + next.suffix.size() : bytes.size();
            after.expanded += fragment_header_bytes + value.size();
            char count[4]; char* cursor = count;
            write32(cursor, after.count);
            cache.remember_inserted(bytes, next.suffix,
                left ? page_header_bytes : bytes.size(), 4, count,
                sizeof(count), member_limit, after);
        }
        plan = std::move(next);
        return status::ok;
    } catch (const std::bad_alloc&) { return status::out_of_memory; }
}

list_page_status list_page_codec::apply_append(std::string& bytes,
    const list_page_edit& plan) {
    return plan.left || plan.right_stride ? status::invalid : apply_edit(bytes, plan);
}

list_page_status list_page_codec::plan_uniform_pop_right(const std::string& bytes,
    uint64_t member_limit, uint16_t verified_count, list_page_update& update,
    list_page_edit& plan) {
    if (plan.id || !is_compact_page(bytes) || bytes.size() > max_page_bytes)
        return status::ok;
    const auto header = bytes[3] == 3 ? size_t(1) : packed_header;
    if (bytes.size() < page_header_bytes + header) return status::ok;
    const auto count = read32(bytes, 4);
    if (count < 2 || count > (max_page_bytes - bytes.size()) /
        (fragment_header_bytes - header)) return status::ok;
    const auto width = read_length(bytes, page_header_bytes, header);
    if (width > member_limit || width > wide_member_bytes)
        return status::ok;
    const auto stride = width + header;
    if (bytes.size() - page_header_bytes != stride * count) return status::ok;
    page_view view{};
    view.id = read64(bytes, 8); view.previous = read64(bytes, 16);
    view.next = read64(bytes, 24); view.count = count; view.header = header;
    if (view.next || !links(view.id, view.previous, view.next)) return status::invalid;
    // A matching hint was published under the same key lock.
    if (count != verified_count && !inspect_uniform(bytes, member_limit, view, nullptr))
        return status::ok;
    const auto start = bytes.size() - stride;
    if (read_length(bytes, start, header) != width) return status::invalid;
    try {
        update.value.assign(bytes, start + header, width);
    } catch (const std::bad_alloc&) { return status::out_of_memory; }
    identify(view, update);
    update.applied = true;
    plan.id = view.id;
    plan.expected_bytes = static_cast<uint32_t>(bytes.size());
    plan.expected_count = static_cast<uint16_t>(count);
    plan.length_bytes = static_cast<uint8_t>(header);
    plan.right_stride = static_cast<uint16_t>(stride);
    return status::ok;
}

list_page_status list_page_codec::truncate_right(std::string& bytes,
    size_t remaining, uint32_t count) {
    try {
        // Release slack before publishing a smaller page.
        if (remaining < bytes.capacity() - bytes.capacity() / 3) {
            std::string smaller(bytes.data(), remaining);
            bytes.swap(smaller);
        } else {
            bytes.resize(remaining);
        }
    } catch (const std::bad_alloc&) { return status::out_of_memory; }
    char* cursor = &bytes[0] + 4;
    write32(cursor, count);
    return status::ok;
}

list_page_status list_page_codec::apply_edit(std::string& bytes,
    const list_page_edit& plan) {
    const auto header = plan.length_bytes;
    if (plan.right_stride) {
        if (header == fragment_header_bytes) return apply_fragment_pop_right(bytes, plan);
        const auto stride = plan.right_stride;
        if (!plan.id || plan.left || plan.expected_count < 2 ||
            (header != 1 && header != packed_header) || stride < header ||
            stride - header > (header == 1 ? size_t(UINT8_MAX) :
                wide_member_bytes) ||
            bytes.size() != plan.expected_bytes ||
            bytes.size() != page_header_bytes + stride * size_t(plan.expected_count) ||
            bytes.size() > max_page_bytes ||
            bytes.compare(0, 4, header == 1 ? "PLP\3" : "PLP\2", 4) ||
            read64(bytes, 8) != plan.id || read64(bytes, 24) ||
            read32(bytes, 4) != plan.expected_count ||
            read_length(bytes, bytes.size() - stride, header) != size_t(stride - header))
            return status::invalid;
        return truncate_right(bytes, bytes.size() - stride, plan.expected_count - 1);
    }
    const auto added = plan.added_bytes();
    if (!plan.id || bytes.size() != plan.expected_bytes ||
        bytes.size() < page_header_bytes || bytes.size() > max_page_bytes ||
        (header != 1 && header != packed_header) ||
        bytes.compare(0, 4, header == 1 ? "PLP\3" : "PLP\2", 4) ||
        read64(bytes, 8) != plan.id ||
        read64(bytes, plan.left ? 16 : 24) ||
        read32(bytes, 4) != plan.expected_count ||
        !plan.expected_count || added < header ||
        (header == 1 && added > size_t(UINT8_MAX) + 1) ||
        (plan.borrowed_value ? plan.borrowed_value == &bytes :
            read_length(plan.suffix, 0, header) != added - header) ||
        added > max_page_bytes - bytes.size()) return status::invalid;
    const auto count = plan.expected_count + uint64_t(1);
    const auto expanded = bytes.size() + added +
        (fragment_header_bytes - header) * count;
    if (expanded > max_page_bytes) return status::invalid;
    try {
        // Reserve is the only potentially throwing step. Until it succeeds,
        // both logical bytes and the matching root remain unchanged.
        const auto needed = bytes.size() + added;
        if (needed > bytes.capacity()) {
            size_t capacity = needed;
            if (added <= list_page_edit::small_member_bytes + 1) {
                const size_t quantum = added > 8 ? 128 : 64;
                capacity = (needed + quantum - 1) / quantum * quantum;
            } else {
                if (header == packed_header &&
                    added >= list_page_edit::reserve_min_width + packed_header &&
                    added <= list_page_edit::reserve_max_width + packed_header && needed >= 1024 &&
                    plan.fill_limit >= expanded &&
                    bytes.size() == page_header_bytes + added * plan.expected_count) {
                    const auto stride = added + fragment_header_bytes - header;
                    const auto remaining = static_cast<size_t>(plan.fill_limit - expanded);
                    capacity = needed + added * (remaining / stride);
                }
#if defined(__APPLE__) && !defined(HAS_JEMALLOC)
                // Use the rest of this allocation class for later pushes.
                capacity = malloc_good_size(capacity + 1) - 1;
#endif
            }
            bytes.reserve(std::min(capacity, size_t(max_page_bytes)));
        }
        if (plan.borrowed_value) {
            const auto width = added - header;
            if (plan.left) {
                const auto before = bytes.size();
                bytes.resize(needed);
                auto* body = &bytes[0] + page_header_bytes;
                std::memmove(body + added, body, before - page_header_bytes);
                write_length(body, static_cast<uint32_t>(width), header);
                std::memcpy(body, plan.borrowed_value->data(), width);
            } else {
                if (header == 1) {
                    bytes.push_back(static_cast<char>(width));
                } else {
                    char length[packed_header];
                    char* cursor = length;
                    write32(cursor, static_cast<uint32_t>(width));
                    bytes.append(length, sizeof(length));
                }
                bytes.append(*plan.borrowed_value);
            }
        } else if (plan.left) bytes.insert(page_header_bytes, plan.suffix);
        else bytes.append(plan.suffix);
        char* cursor = &bytes[0] + 4;
        write32(cursor, static_cast<uint32_t>(count));
        return status::ok;
    } catch (const std::bad_alloc&) { return status::out_of_memory; }
}

list_page_status list_page_codec::push_compact(const std::string& bytes,
    list_values_view values, bool left, uint64_t member_limit,
    list_page_update& out, size_t fill_target, uint64_t neighbor) {
    using validation_cache = detail::validated_page_cache<page_view>;
    const bool eligible = bytes.size() >= page_header_bytes &&
        bytes.size() <= validation_cache::max_bytes && read32(bytes, 4) >= 8;
    validation_cache cache(eligible);
    page_view view;
    const bool valid = cache.lookup(bytes, member_limit, view) ||
        inspect(bytes, member_limit, view);
    if (!valid ||
        (left ? view.previous != 0 : view.next != 0)) return status::invalid;
    list_page_update update;
    identify(view, update);
    const auto limit = view.expanded > fill_target ? max_page_bytes :
        fill_target;
    size_t expanded = view.expanded, size = bytes.size();
    bool fits = !values.empty();
    bool widen = false;
    for (const auto& value : values) {
        if (value.size() > member_limit) return status::invalid;
        if (fragment_header_bytes > limit - expanded ||
            value.size() > limit - expanded - fragment_header_bytes)
            fits = false;
        widen = widen || (view.header == 1 && value.size() > UINT8_MAX);
        if (fits) {
            expanded += fragment_header_bytes + value.size();
            size += view.header + value.size();
        }
    }
    if (!fits) {
        // If append would start a fresh page, patch the validated old link.
        if (neighbor && values.size() == 1 &&
            links(view.id, left ? neighbor : view.previous,
                left ? view.next : neighbor)) {
            const auto available = limit - view.expanded;
            const auto width = values[0].size();
            if (available < fragment_header_bytes + (width != 0) ||
                prefer_whole_member(width, fill_target)) {
                try {
                    update.bytes = bytes;
                    char* link = &update.bytes[0] + (left ? 16 : 24);
                    write64(link, neighbor);
                    update.linked = true;
                } catch (const std::bad_alloc&) { return status::out_of_memory; }
            }
        }
        out = std::move(update);
        return status::ok;
    }
    if (widen) {
        out = std::move(update);
        return status::ok;
    }
    try {
        detail::node_write_workspace workspace(
            detail::node_write_workspace::fits(size));
        auto& encoded = workspace.buffer();
        encoded.reserve(size);
        encoded.append(bytes.data(), left ? page_header_bytes : bytes.size());
        for (size_t i = 0; i < values.size(); ++i) {
            const auto& value = values[left ? values.size() - i - 1 : i];
            char header[packed_header];
            char* cursor = header;
            write_length(cursor, static_cast<uint32_t>(value.size()), view.header);
            encoded.append(header, view.header); encoded.append(value);
        }
        if (left) encoded.append(bytes, page_header_bytes, std::string::npos);
        char* cursor = &encoded[0] + 4;
        write32(cursor, view.count + static_cast<uint32_t>(values.size()));
        if (view.count + values.size() >= 8 &&
            size <= validation_cache::max_bytes) {
            auto next = view;
            next.count += static_cast<uint32_t>(values.size());
            next.expanded = expanded;
            next.last = left ? view.last + size - bytes.size() :
                size - view.header - values.back().size();
            cache.activate();
            cache.remember(encoded, member_limit, next);
        }
        update.bytes.swap(encoded); update.applied = true;
        out = std::move(update); return status::ok;
    } catch (const std::bad_alloc&) { return status::out_of_memory; }
}

list_page_status list_page_codec::pop_compact(const std::string& bytes,
    bool left, uint64_t member_limit, list_page_update& out) {
    if (!left) return pop_compact_right(bytes, member_limit, out);
    using validation_cache = detail::validated_page_cache<page_view>;
    // An address is only an admission hint. Cache hits still require
    // the full byte comparison, even if storage reused this address.
    static thread_local uintptr_t last_page = 0;
    const auto address = reinterpret_cast<uintptr_t>(&bytes);
    const bool repeated = last_page == address;
    last_page = address;
    const bool eligible = repeated && bytes.size() >= page_header_bytes &&
        bytes.size() <= validation_cache::max_bytes && read32(bytes, 4) >= 8;
    validation_cache cache(eligible);
    page_view view;
    const bool valid = cache.lookup(bytes, member_limit, view) ||
        inspect(bytes, member_limit, view);
    if (!valid || view.previous) return status::invalid;
    list_page_update update;
    identify(view, update);
    if (view.count == 1) { out = std::move(update); return status::ok; }
    const auto first = page_header_bytes;
    const auto size = read_length(bytes, first, view.header);
    const size_t removed = view.header + size;
    try {
        // The surviving body is contiguous. For a left pop, copy the
        // equally sized suffix and restore its header from the original.
        const auto size_after = bytes.size() - removed;
        detail::node_write_workspace workspace(
            detail::node_write_workspace::fits(size_after));
        auto& encoded = workspace.buffer();
        encoded.assign(bytes.data() + removed, size_after);
        std::memcpy(&encoded[0], bytes.data(), page_header_bytes);
        char* cursor = &encoded[0] + 4; write32(cursor, view.count - 1);
        update.value.assign(bytes, first + view.header, size);
        if (repeated && view.count > 8 &&
            encoded.size() <= validation_cache::max_bytes) {
            --view.count;
            view.expanded -= fragment_header_bytes + size;
            view.last -= removed;
            cache.activate();
            cache.remember(encoded, member_limit, view);
        }
        update.bytes.swap(encoded); update.applied = true;
        out = std::move(update); return status::ok;
    } catch (const std::bad_alloc&) { return status::out_of_memory; }
}

list_page_status list_page_codec::pop_compact_right(const std::string& bytes,
    uint64_t member_limit, list_page_update& out) {
    const bool eligible = admit_right_cache(bytes);
    right_validation_cache cache(eligible);
    right_page_view indexed{};
    auto& view = indexed.page;
    batch_offsets bounds(0);
    const bool valid = cache.lookup(bytes, member_limit, indexed) ||
        (eligible ? inspect(bytes, member_limit, view, &bounds, indexed.offsets) :
            inspect(bytes, member_limit, view));
    if (!valid || view.next) return status::invalid;
    list_page_update update;
    identify(view, update);
    if (view.count == 1) { out = std::move(update); return status::ok; }
    const auto size = read_length(bytes, view.last, view.header);
    try {
        detail::node_write_workspace workspace(
            detail::node_write_workspace::fits(view.last));
        auto& encoded = workspace.buffer();
        encoded.assign(bytes.data(), view.last);
        char* cursor = &encoded[0] + 4;
        write32(cursor, view.count - 1);
        update.value.assign(bytes, view.last + view.header, size);
        if (eligible && !bounds.stride && view.count > 32)
            remember_right_prefix(cache, bytes, encoded, member_limit, indexed, 1);
        update.bytes.swap(encoded); update.applied = true;
        out = std::move(update); return status::ok;
    } catch (const std::bad_alloc&) { return status::out_of_memory; }
}
list_page_status list_page_codec::pop_compact_batch(const std::string& bytes,
    bool left, uint64_t count, uint64_t member_limit, list_page_update& out,
    std::vector<std::string>* values, uint64_t removed_neighbor, bool allow_whole) {
    if (bytes.size() < page_header_bytes || !codec::is_compact_page(bytes)) {
        out = list_page_update();
        return status::ok;
    }
    const auto members = read32(bytes, 4);
    const bool whole = count >= members;
    if (whole && !removed_neighbor && !allow_whole) {
        out = list_page_update();
        return status::ok;
    }
    if (whole) count = members;
    const bool eligible = count && !whole && !left && admit_right_cache(bytes);
    right_validation_cache cache(eligible);
    right_page_view indexed{};
    auto& view = indexed.page;
    const auto first = left || !count ? 0 : static_cast<uint32_t>(members - count);
    batch_offsets batch(first);
    if ((!count && !removed_neighbor) || !(cache.lookup(bytes, member_limit, indexed) ||
        inspect(bytes, member_limit, view, &batch, eligible ? indexed.offsets : nullptr)) ||
        (left ? view.previous : view.next) != removed_neighbor) return status::invalid;
    if (!count && view.header == packed_header &&
        bytes.size() - page_header_bytes <= view.count * (packed_header + UINT8_MAX)) {
        // The existing writer can narrow a PLP2 page of short members.
        out = list_page_update();
        return status::ok;
    }
    const auto stride = batch.stride;
    const auto start = eligible && !stride ?
        member_offset(bytes, indexed, first) : batch.start;
    size_t cursor = start;
    std::vector<std::string> local;
    auto& popped = removed_neighbor && values ? *values : local;
    const auto prefix = popped.size();
    try {
        if (values && !removed_neighbor) popped.reserve(static_cast<size_t>(count));
        if (stride) {
            if (values) {
                const auto width = stride - view.header;
                for (uint64_t i = 0; i < count; ++i) {
                    const auto index = left ? i : count - 1 - i;
                    popped.emplace_back(bytes.data() + start + index * stride +
                        view.header, width);
                }
            }
            cursor += count * stride;
        } else {
            for (uint64_t i = 0; i < count; ++i) {
                const auto size = read_length(bytes, cursor, view.header);
                if (values) popped.emplace_back(bytes.data() + cursor + view.header,
                    size);
                cursor += view.header + size;
            }
            if (!left && values) std::reverse(popped.begin() + prefix, popped.end());
        }
        list_page_update update;
        identify(view, update);
        if (whole) {
            update.consumed = view.count;
            if (values && !removed_neighbor) values->swap(popped);
            out = std::move(update);
            return status::ok;
        }
        const auto removed = cursor - start;
        const auto size_after = bytes.size() - removed;
        detail::node_write_workspace workspace(
            detail::node_write_workspace::fits(size_after));
        auto& encoded = workspace.buffer();
        encoded.assign(bytes.data() + (left ? removed : 0), size_after);
        if (left) std::memcpy(&encoded[0], bytes.data(), page_header_bytes);
        char* count_bytes = &encoded[0] + 4;
        write32(count_bytes, view.count - static_cast<uint32_t>(count));
        if (removed_neighbor) {
            char* link = &encoded[0] + (left ? 16 : 24);
            write64(link, 0);
        }
        if (!removed_neighbor && eligible && !stride && view.count - count >= 32)
            remember_right_prefix(cache, bytes, encoded, member_limit, indexed,
                static_cast<uint32_t>(count));
        update.bytes.swap(encoded); update.applied = true;
        if (values && !removed_neighbor) values->swap(popped);
        out = std::move(update);
        return status::ok;
    } catch (const std::bad_alloc&) {
        popped.resize(prefix);
        return status::out_of_memory;
    }
}
} // namespace pkv
