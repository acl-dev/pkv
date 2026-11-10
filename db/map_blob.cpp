#include "map_store.h"
#include <algorithm>
#include <cstring>

namespace pkv {
namespace {
bool valid_link(const map_fragment_view& page, uint64_t id,
    uint64_t offset, uint64_t length, uint64_t last_id) {
    return page.id == id && page.next <= last_id && page.offset == offset &&
        offset <= length && page.size <= length - offset &&
        ((page.size == length - offset) == (page.next == 0));
}
}

map_store::blob_reader::blob_reader(map_store& store, const map_blob& blob)
: store_(store), blob_(blob), id_(blob.head) {}

bool map_store::blob_reader::next(map_fragment_view& page, bool& present) {
    present = false;
    if (store_.error_ != status::ok) return false;
    if (!inline_done_ && !blob_.inline_value.empty()) {
        page = map_fragment_view();
        page.data = blob_.inline_value.data();
        page.size = blob_.inline_value.size();
        inline_done_ = true; present = true;
        return true;
    }
    if (!blob_.head) return true;
    const auto length = blob_.length - blob_.inline_value.size();
    if (!id_) return offset_ == length || store_.fail(status::invalid);
    map_fragment_view value;
    if (!store_.read(id_, bytes_, true) || !store_.decoded(
        map_page_codec::decode_fragment_view(bytes_, value))) return false;
    if (!valid_link(value, id_, offset_, length, store_.last_id()))
        return store_.fail(status::invalid);
    offset_ += value.size; id_ = value.next;
    page = value; present = true;
    return true;
}

bool map_store::visit_blob(const map_blob& blob,
    const fragment_visitor& callback) {
    if (!blob.head) return true;
    blob_reader reader(*this, blob);
    map_fragment_view page;
    bool present;
    while (reader.next(page, present)) {
        if (!present) return true;
        if (!callback(page)) return error_ == status::ok;
    }
    return false;
}

bool map_store::compare_blobs(const map_blob& left, const map_blob& right,
    int& order) {
    blob_reader first(*this, left), second(*this, right);
    map_fragment_view a, b;
    size_t offset_a = 0, offset_b = 0;
    bool have_a = true, have_b = true;
    order = 0;
    for (;;) {
        if (offset_a == a.size) {
            if (!first.next(a, have_a)) return false;
            offset_a = 0;
        }
        if (offset_b == b.size) {
            if (!second.next(b, have_b)) return false;
            offset_b = 0;
        }
        if (!have_a || !have_b) {
            order = have_a ? 1 : have_b ? -1 : 0;
            return true;
        }
        const auto count = std::min(a.size - offset_a,
            b.size - offset_b);
        order = std::memcmp(a.data + offset_a,
            b.data + offset_b, count);
        if (order) return true;
        offset_a += count; offset_b += count;
    }
}

bool map_store::compare(const map_blob& key, const std::string& input,
    int& order, bool* matched_single_fragment) {
    order = 0;
    if (!key.head) {
        order = key.inline_value.compare(input);
        return true;
    }
    const auto prefix = key.inline_value.size();
    if (prefix) {
        order = std::memcmp(key.inline_value.data(), input.data(),
            std::min(prefix, input.size()));
        if (order || input.size() <= prefix) {
            if (!order) order = 1;
            if (matched_single_fragment) *matched_single_fragment = false;
            return true;
        }
    }
    struct comparison {
        const std::string& input;
        uint64_t length, last_id;
        size_t prefix;
        uint64_t id, offset = 0;
        int order = 0;
        map_page_status status = map_page_status::invalid;
    } job{input, key.length - prefix, last_id(), prefix, key.head};
    const auto consume = [](void* raw, const std::string& bytes) {
        auto& job = *static_cast<comparison*>(raw);
        map_fragment_view page;
        job.status = map_page_codec::decode_fragment_view(bytes, page);
        if (job.status != map_page_status::ok) return;
        if (!valid_link(page, job.id, job.offset, job.length, job.last_id)) {
            job.status = map_page_status::invalid;
            return;
        }
        const auto offset = static_cast<size_t>(job.offset) + job.prefix;
        if (offset >= job.input.size()) {
            job.order = 1;
            return;
        }
        const auto length = std::min(page.size, job.input.size() - offset);
        job.order = std::memcmp(page.data, job.input.data() + offset, length);
        if (!job.order) {
            if (page.next && offset + length == job.input.size()) job.order = 1;
            else if (length < page.size) job.order = 1;
            else if (!page.next && offset + length < job.input.size()) job.order = -1;
        }
        job.offset += page.size;
        job.id = page.next;
    };
    std::string scratch;
    if (!inspect(job.id, scratch, consume, &job) || !decoded(job.status)) return false;
    if (matched_single_fragment)
        *matched_single_fragment = !prefix && !job.id && !job.order;
    while (job.id && !job.order) {
        if (!inspect(job.id, scratch, consume, &job) || !decoded(job.status)) return false;
    }
    order = job.order;
    return true;
}

bool map_store::store_blob(const std::string& input, size_t limit,
    map_blob& out, uint64_t retired_head) {
    // Reuse a fitting bounded buffer without growing its allocation class.
    // assign also permits input to alias the existing inline value. Publish
    // the scalar fields only after the potentially throwing string operation.
    if (input.size() <= limit && out.inline_value.capacity() >= input.size() &&
        out.inline_value.capacity() <= limit + 15) {
        out.inline_value.assign(input);
        out.length = input.size(); out.head = 0;
        return true;
    }
    map_blob blob;
    blob.length = input.size();
    if (input.size() <= limit) {
        blob.inline_value = input;
        out = std::move(blob);
        return true;
    }
    constexpr auto capacity = map_page_codec::max_page_bytes -
        map_page_codec::fragment_header_bytes;
    size_t spare = 0;
#if defined(__APPLE__) && !defined(HAS_JEMALLOC)
    // Use the final page's spare space to keep preceding pages in the
    // 16 KiB allocation class, including std::string's terminator.
    if (input.size() > capacity) {
        const auto tail = input.size() % capacity;
        if (tail) spare = capacity - tail;
    }
#endif
    uint64_t id = retired_head;
    if (!id && !allocate(id)) return false;
    blob.head = id;
    size_t offset = 0;
    while (offset < input.size()) {
        const bool leave_terminator = spare != 0;
        const auto length = std::min(capacity - leave_terminator, input.size() - offset);
        spare -= leave_terminator;
        uint64_t next = 0;
        if (offset + length < input.size() && !allocate(next)) return false;
        std::string bytes;
        const bool compact = input.size() <= 512 && (kind_ == map_kind::set ?
            transaction_.database_.supports_compact_set_fragments() :
            embedded_ && bound_bucket_ == 256 && transaction_.database_.supports_compact_zsets());
        if (!decoded(map_page_codec::encode_fragment(id, next, offset,
            input.data() + offset, length, bytes, compact)))
            return false;
        if (!transaction_.put_bound(entry_, id, std::move(bytes)))
            return fail(status::storage);
        offset += length;
        id = next;
    }
    out = std::move(blob);
    return true;
}

bool map_store::read_blob(const map_blob& blob, std::string& out, bool take) {
    if (!blob.head) { out = blob.inline_value; return true; }
    std::string value;
    const bool shared = blob.shares_fragments();
    if (shared) {
        value.reserve(static_cast<size_t>(blob.length));
        value.assign(blob.inline_value);
        take = false;
    } else if (embedded_ && bound_bucket_ == 256 && blob.length <= 512) {
        struct read_job {
            const map_blob& blob;
            std::string& value;
            uint64_t last;
            map_page_status result;
        } job{blob, value, last_id(), map_page_status::invalid};
        const auto consume = [](void* raw, const std::string& bytes) {
            auto& job = *static_cast<read_job*>(raw);
            map_fragment_view page;
            job.result = map_page_codec::decode_fragment_view(bytes, page);
            if (job.result != map_page_status::ok) return;
            if (!valid_link(page, job.blob.head, 0, job.blob.length, job.last)) {
                job.result = map_page_status::invalid;
                return;
            }
            if (!page.next) job.value.assign(page.data, page.size);
        };
        std::string scratch;
        if (!inspect(blob.head, scratch, consume, &job) || !decoded(job.result))
            return false;
        if (!value.empty()) {
            if (take && !transaction_.erase_bound(entry_, blob.head))
                return fail(status::storage);
            out.swap(value);
            return true;
        }
        // A short payload from an older writer can still span fragments.
    }
    struct read_job {
        std::string& value;
        uint64_t length, last;
        uint64_t id, offset = 0;
        map_page_status result = map_page_status::invalid;
    } job{value, blob.length - blob.inline_value.size(), last_id(), blob.head};
    const auto consume = [](void* raw, const std::string& bytes) {
        auto& job = *static_cast<read_job*>(raw);
        map_fragment_view page;
        job.result = map_page_codec::decode_fragment_view(bytes, page);
        if (job.result != map_page_status::ok) return;
        if (!valid_link(page, job.id, job.offset, job.length, job.last)) {
            job.result = map_page_status::invalid;
            return;
        }
        if (job.value.empty() && page.next) job.value.reserve(static_cast<size_t>(job.length));
        job.value.append(page.data, page.size);
        job.offset += page.size;
        job.id = page.next;
    };
    std::string scratch;
    while (job.id) {
        const auto id = job.id;
        if (!inspect(id, scratch, consume, &job) || !decoded(job.result))
            return false;
        // Release the borrowed view before staging any deletion.
        if (take && !transaction_.erase_bound(entry_, id)) return fail(status::storage);
    }
    out.swap(value);
    return true;
}

bool map_store::update_fragments(const map_blob& blob, const std::string* replacement) {
    if (!blob.head) return true;
    if (blob.shares_fragments()) return !replacement || fail(status::invalid);
    struct inspection {
        map_fragment_view page;
        map_page_status status;
    } fragment{};
    const auto decode = [](void* raw, const std::string& bytes) {
        auto& fragment = *static_cast<inspection*>(raw);
        fragment.status = map_page_codec::decode_fragment_view(bytes, fragment.page);
        // Only the header and payload length survive the borrowed view.
        fragment.page.data = nullptr;
    };
    std::string scratch;
    const auto limit = last_id();
    uint64_t offset = 0;
    for (auto id = blob.head; id; id = fragment.page.next) {
        if (!inspect(id, scratch, decode, &fragment) || !decoded(fragment.status))
            return false;
        if (!valid_link(fragment.page, id, offset, blob.length, limit))
            return fail(status::invalid);
        // Stage only after the backend has released its borrowed view.
        if (replacement) {
            std::string bytes;
            if (!decoded(map_page_codec::encode_fragment(id, fragment.page.next,
                offset, replacement->data() + offset, fragment.page.size, bytes)))
                return false;
            if (!transaction_.put_bound(entry_, id, std::move(bytes)))
                return fail(status::storage);
        } else if (!transaction_.erase_bound(entry_, id)) {
            return fail(status::storage);
        }
        offset += fragment.page.size;
    }
    return true;
}

} // namespace pkv
