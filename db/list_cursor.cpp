#include "list_store.h"
#include "common/redis_list_util.h"
#include <algorithm>
#include <cstring>
#include <climits>

namespace pkv {

list_store::cursor::cursor(list_store& store, bool reverse)
: store_(store)
, reverse_(reverse)
, next_(reverse ? store.root_.tail : store.root_.head)
{}

const list_fragment* list_store::cursor::fragment() {
    if (position_ == 0) {
        if (!next_ || !visited_.insert(next_).second ||
            !store_.read_page(next_, page_)) {
            store_.fail(status::invalid);
            return nullptr;
        }
        if ((reverse_ ? page_.next : page_.previous) != previous_) {
            store_.fail(status::invalid);
            return nullptr;
        }
        previous_ = page_.id;
        next_ = reverse_ ? page_.previous : page_.next;
    }
    const auto index = reverse_ ? page_.fragments.size() - position_ - 1 :
        position_;
    current_.page = page_.id;
    current_.slot = index;
    const auto* result = &page_.fragments[index];
    ++position_;
    if (position_ == page_.fragments.size()) position_ = 0;
    return result;
}

bool list_store::cursor::member(std::string* out, const std::string* match,
    bool* matched, span* where) {
    const auto* item = fragment();
    if (!item) return false;
    const auto start = current_;
    const auto total = item->total_bytes;
    if (reverse_ ? item->offset + item->bytes.size() != total :
        item->offset != 0) return store_.fail(status::invalid);
    uint64_t offset = reverse_ ? total : 0;
    if (out) out->assign(static_cast<size_t>(total), '\0');
    if (matched) *matched = match && match->size() == total;
    while (true) {
        if (item->total_bytes != total || (reverse_ ?
            item->offset + item->bytes.size() != offset :
            item->offset != offset)) return store_.fail(status::invalid);
        if (matched && *matched && match->compare(
            static_cast<size_t>(item->offset), item->bytes.size(),
            item->bytes) != 0) *matched = false;
        if (out && !item->bytes.empty()) {
            std::memcpy(&(*out)[static_cast<size_t>(item->offset)],
                item->bytes.data(), item->bytes.size());
        }
        offset = reverse_ ? item->offset : offset + item->bytes.size();
        if (reverse_ ? offset == 0 : offset == total) {
            if (where) {
                where->first = reverse_ ? current_ : start;
                where->last = reverse_ ? start : current_;
            }
            return true;
        }
        item = fragment();
        if (!item) return false;
    }
}

bool list_store::compact_range(uint64_t skip, uint64_t count, bool reverse,
    std::vector<std::string>& values, bool& handled) {
    handled = false;
    uint64_t id = reverse ? root_.tail : root_.head, previous = 0;
    std::string scratch;
    list_page_update page;
    struct request {
        uint64_t skip, take;
        bool reverse;
        uint32_t members;
        list_page_status status;
        list_page_update& page;
        std::vector<std::string>& values;
    } job{skip, count, reverse, 0, list_page_status::invalid, page, values};
    const auto read = [](void* raw, const std::string& bytes) {
        auto& task = *static_cast<request*>(raw);
        task.status = list_page_codec::range_compact(bytes, task.skip, task.take,
            task.reverse, max_member_bytes, task.members, task.page, task.values);
    };
    while (id) {
        if (!inspect(id, scratch, read, &job)) return false;
        if (job.status != list_page_status::ok)
            return fail(job.status == list_page_status::out_of_memory ?
                status::storage : status::invalid);
        if (!job.members) {
            values.clear();
            return true;
        }
        if (page.id != id || !check_links(id, page.previous, page.next) ||
            (reverse ? page.next : page.previous) != previous)
            return fail(status::invalid);
        if (values.size() == count) {
            handled = true;
            return true;
        }
        job.skip -= std::min<uint64_t>(job.skip, job.members);
        job.take = count - values.size();
        previous = id;
        id = reverse ? page.previous : page.next;
    }
    return fail(status::invalid);
}

bool list_store::range(long long start, long long end,
    std::vector<std::string>& out) {
    out.clear();
    if (error_ != status::ok) return false;
    size_t offset, count;
    if (!list_slice(static_cast<size_t>(root_.elements), start, end, offset,
        count)) return true;
    // Walk from the nearer end; skipped members do not allocate value buffers.
    const auto after = static_cast<size_t>(root_.elements) - offset - count;
    const bool reverse = after < offset;
    const auto skip = reverse ? after : offset;
    std::vector<std::string> values;
    values.reserve(std::min(count, size_t(1024)));
    bool handled;
    if (!compact_range(skip, count, reverse, values, handled)) return false;
    if (!handled) {
        cursor reader(*this, reverse);
        for (size_t i = 0; i < skip; ++i) {
            if (!reader.member(nullptr)) return false;
        }
        for (size_t i = 0; i < count; ++i) {
            std::string value;
            if (!reader.member(&value)) return false;
            values.push_back(std::move(value));
        }
    }
    if (reverse) std::reverse(values.begin(), values.end());
    out.swap(values);
    return true;
}

bool list_store::compact_index(uint64_t offset, list_page_update& result,
    bool& handled, const std::string* replacement) {
    handled = false;
    const auto after = root_.elements - offset - 1;
    const bool reverse = after < offset;
    uint64_t skip = reverse ? after : offset;
    uint64_t id = reverse ? root_.tail : root_.head, previous = 0;
    std::string scratch;
    struct request {
        list_page_update& result;
        uint64_t skip;
        bool reverse;
        const std::string* replacement;
        uint32_t count;
        list_page_status status;
    } job{result, skip, reverse, replacement, 0, list_page_status::invalid};
    const auto read = [](void* raw, const std::string& bytes) {
        auto& task = *static_cast<request*>(raw);
        task.status = list_page_codec::index_compact(bytes, task.skip,
            task.reverse, max_member_bytes, task.count, task.result, task.replacement);
    };
    while (id) {
        job.skip = skip;
        if (!inspect(id, scratch, read, &job)) return false;
        if (job.status != list_page_status::ok)
            return fail(job.status == list_page_status::out_of_memory ?
                status::storage : status::invalid);
        if (!job.count) return true;
        if (result.id != id || !check_links(id, result.previous, result.next) ||
            (reverse ? result.next : result.previous) != previous)
            return fail(status::invalid);
        if (skip < job.count) {
            handled = !replacement || result.applied;
            return true;
        }
        skip -= job.count;
        previous = id;
        id = reverse ? result.previous : result.next;
    }
    return fail(status::invalid);
}

bool list_store::index(long long member_index, std::string& out, bool& found) {
    found = false;
    if (error_ != status::ok) return false;
    size_t offset;
    if (!list_index(static_cast<size_t>(root_.elements), member_index, offset)) {
        return true;
    }
    list_page_update result;
    bool handled;
    if (!compact_index(offset, result, handled)) return false;
    if (!handled) {
        // A fragmented page may contain only part of a member.
        const auto after = root_.elements - offset - 1;
        const bool reverse = after < offset;
        cursor reader(*this, reverse);
        for (uint64_t i = 0; i < (reverse ? after : offset); ++i) {
            if (!reader.member(nullptr)) return false;
        }
        if (!reader.member(&result.value)) return false;
    }
    out.swap(result.value);
    found = true;
    return true;
}

bool list_store::trim(long long start, long long end) {
    if (error_ != status::ok) return false;
    if (root_.elements) dirty_ = true;
    size_t offset, count;
    if (!list_slice(static_cast<size_t>(root_.elements), start, end, offset,
        count)) return clear();
    const auto after = root_.elements - offset - count;
    return pop(after, false, nullptr) && pop(offset, true, nullptr);
}

bool list_store::positions(const std::string& value, const list_search& options,
    std::vector<long long>& out) {
    out.clear();
    if (error_ != status::ok) return false;
    if (options.rank == 0 || options.rank == LLONG_MIN || options.count < 0 ||
        options.maxlen < 0) return fail(status::invalid_argument);
    const bool reverse = options.rank < 0;
    uint64_t skip = static_cast<uint64_t>(reverse ? -(options.rank + 1) :
        options.rank - 1);
    const auto limit = options.maxlen ? std::min(root_.elements,
        static_cast<uint64_t>(options.maxlen)) : root_.elements;
    cursor reader(*this, reverse);
    std::vector<long long> positions;
    for (uint64_t scanned = 0; scanned < limit; ++scanned) {
        bool matched = false;
        if (!reader.member(nullptr, &value, &matched)) return false;
        if (!matched) continue;
        if (skip) { --skip; continue; }
        positions.push_back(static_cast<long long>(reverse ?
            root_.elements - scanned - 1 : scanned));
        if (options.count && positions.size() >=
            static_cast<uint64_t>(options.count)) break;
    }
    out.swap(positions);
    return true;
}

} // namespace pkv
