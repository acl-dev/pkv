#include "list_store.h"
#include <algorithm>
#include <climits>

namespace pkv {

list_store::segment_writer::segment_writer(list_store& store, uint64_t id,
    uint64_t previous)
: store_(store), first_(id) {
    page_.id = id;
    page_.previous = previous;
}

bool list_store::segment_writer::grow() {
    list_page next;
    if (!store_.allocate(next)) return false;
    next.previous = page_.id;
    page_.next = next.id;
    if (!store_.write_page(page_)) return false;
    page_ = std::move(next);
    used_ = list_page_codec::page_header_bytes;
    return true;
}

bool list_store::segment_writer::fragment(const list_fragment& value) {
    const auto bytes = list_page_codec::fragment_header_bytes +
        value.bytes.size();
    if (bytes > list_page_codec::max_page_bytes - used_ && !grow()) {
        return false;
    }
    page_.fragments.push_back(value);
    used_ += bytes;
    nonempty_ = true;
    return true;
}

bool list_store::segment_writer::value(const std::string& value) {
    size_t offset = 0;
    bool first = true;
    while (first || offset < value.size()) {
        const auto header = list_page_codec::fragment_header_bytes;
        const auto available = list_page_codec::max_page_bytes - used_;
        if (available < header + (value.empty() ? 0 : 1)) {
            if (!grow()) return false;
            continue;
        }
        list_fragment part;
        part.total_bytes = value.size();
        part.offset = offset;
        part.bytes.assign(value, offset, std::min(value.size() - offset,
            available - header));
        offset += part.bytes.size();
        if (!fragment(part)) return false;
        first = false;
    }
    return true;
}

bool list_store::segment_writer::finish(uint64_t next) {
    if (!nonempty_) return true;
    page_.next = next;
    return store_.write_page(page_);
}

bool list_store::locate(uint64_t index, span& out) {
    const auto after = root_.elements - index - 1;
    const bool reverse = after < index;
    cursor reader(*this, reverse);
    for (uint64_t i = 0, skip = reverse ? after : index; i < skip; ++i) {
        if (!reader.member(nullptr)) return false;
    }
    return reader.member(nullptr, nullptr, nullptr, &out);
}

bool list_store::neighbors(const list_page& first, const list_page& last,
    uint64_t new_first, uint64_t new_last) {
    const auto left_target = new_first ? new_first : last.next;
    const auto right_target = new_last ? new_last : first.previous;
    if (first.previous && left_target != first.id) {
        list_page previous;
        if (!read_page(first.previous, previous) || previous.next != first.id) {
            return fail(status::invalid);
        }
        previous.next = left_target;
        if (!write_page(previous)) return false;
    }
    if (last.next && right_target != last.id) {
        list_page next;
        if (!read_page(last.next, next) || next.previous != last.id) {
            return fail(status::invalid);
        }
        next.previous = right_target;
        if (!write_page(next)) return false;
    }
    if (!first.previous) root_.head = new_first ? new_first : last.next;
    if (!last.next) root_.tail = new_last ? new_last : first.previous;
    return true;
}

bool list_store::splice(position begin, position end,
    const std::string* value, uint64_t removed) {
    dirty_ = true;
    if (!counter()) return false;
    list_page first, last;
    if (!read_page(begin.page, first)) return false;
    std::vector<uint64_t> discarded;
    std::unordered_set<uint64_t> visited;
    auto id = begin.page;
    uint64_t previous = first.previous;
    last = first;
    while (true) {
        if (!id || !visited.insert(id).second ||
            (id != begin.page && !read_page(id, last)) ||
            last.previous != previous) return fail(status::invalid);
        discarded.push_back(id);
        if (id == end.page) break;
        previous = id;
        id = last.next;
    }
    if (begin.slot > first.fragments.size() || end.slot > last.fragments.size()
        || (begin.page == end.page && begin.slot > end.slot) ||
        visited.count(first.previous) || visited.count(last.next)) {
        return fail(status::invalid);
    }
    segment_writer writer(*this, first.id, first.previous);
    for (size_t i = 0; i < begin.slot; ++i) {
        if (!writer.fragment(first.fragments[i])) return false;
    }
    if (value && !writer.value(*value)) return false;
    for (size_t i = end.slot; i < last.fragments.size(); ++i) {
        if (!writer.fragment(last.fragments[i])) return false;
    }
    if (!writer.finish(last.next)) return false;
    for (const auto page : discarded) {
        if (page != writer.first() && !transaction_.erase_bound(entry_, page)) {
            return fail(status::storage);
        }
    }
    if (!neighbors(first, last, writer.first(), writer.last())) return false;
    root_.elements -= removed;
    if (value) ++root_.elements;
    if ((root_.elements == 0) != (root_.head == 0 && root_.tail == 0)) {
        return fail(status::invalid);
    }
    return true;
}

bool list_store::replace(long long position, const std::string& value,
    bool& found) {
    found = false;
    if (error_ != status::ok) return false;
    if (value.size() > max_member_bytes) return fail(status::too_large);
    size_t index;
    if (!list_index(static_cast<size_t>(root_.elements), position, index)) {
        return true;
    }
    list_page_update update;
    bool handled;
    if (!compact_index(index, update, handled, &value)) return false;
    if (handled) {
        if (!counter()) return false;
        if (!transaction_.put_bound(entry_, update.id, std::move(update.bytes)))
            return fail(status::storage);
        dirty_ = true;
        found = true;
        return true;
    }
    span target;
    if (!locate(index, target)) return false;
    ++target.last.slot;
    if (!splice(target.first, target.last, &value, 1)) return false;
    found = true;
    return true;
}

bool list_store::insert(bool before, const std::string& pivot,
    const std::string& value, long long& length) {
    length = 0;
    if (error_ != status::ok) return false;
    if (value.size() > max_member_bytes) return fail(status::too_large);
    if (!root_.elements) return true;
    cursor reader(*this, false);
    for (uint64_t i = 0; i < root_.elements; ++i) {
        span target;
        bool matched;
        if (!reader.member(nullptr, &pivot, &matched, &target)) return false;
        if (!matched) continue;
        if (root_.elements == static_cast<uint64_t>(LLONG_MAX)) {
            return fail(status::too_large);
        }
        position point = before ? target.first : target.last;
        if (!before) ++point.slot;
        if (!splice(point, point, &value, 0)) return false;
        length = static_cast<long long>(root_.elements);
        return true;
    }
    length = -1;
    return true;
}

bool list_store::clear() {
    if (error_ != status::ok) return false;
    if (!root_.elements) return true;
    dirty_ = true;
    if (!transaction_.reset_pages(owner_)) return fail(status::storage);
    root_.elements = root_.head = root_.tail = 0;
    entry_->action = db::update_action::erase;
    entry_->clear_pages = false;
    return true;
}

bool list_store::remove(long long count, const std::string& value,
    long long& removed) {
    removed = 0;
    if (error_ != status::ok) return false;
    if (count == LLONG_MIN) return fail(status::invalid_argument);
    struct run { span where; uint64_t count = 0; };
    const bool reverse = count < 0;
    const auto limit = static_cast<uint64_t>(reverse ? -count : count);
    cursor reader(*this, reverse);
    std::vector<run> runs;
    bool adjacent = false;
    for (uint64_t i = 0; i < root_.elements; ++i) {
        span target;
        bool matched;
        if (!reader.member(nullptr, &value, &matched, &target)) return false;
        if (!matched) { adjacent = false; continue; }
        if (!adjacent) {
            run next;
            next.where = target;
            runs.push_back(next);
        } else if (reverse) {
            runs.back().where.first = target.first;
        } else {
            runs.back().where.last = target.last;
        }
        adjacent = true;
        ++runs.back().count;
        ++removed;
        if (limit && static_cast<uint64_t>(removed) == limit) break;
    }
    if (static_cast<uint64_t>(removed) == root_.elements) return clear();
    if (!reverse) std::reverse(runs.begin(), runs.end());
    for (const auto& run : runs) {
        auto end = run.where.last;
        ++end.slot;
        if (!splice(run.where.first, end, nullptr, run.count)) return false;
    }
    return true;
}

} // namespace pkv
