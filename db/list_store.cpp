#include "coder/compact_list_root.h"
#include "list_store.h"
#include "common/redis_list_util.h"
#include <algorithm>
#include <climits>
#include <cstring>

namespace pkv {

const uint64_t list_store::max_member_bytes;

namespace {

size_t push_target(list_values_view values) {
    const size_t maximum = list_page_codec::max_page_bytes;
    const size_t header = list_page_codec::fragment_header_bytes;
    const size_t ceiling = (maximum - list_page_codec::page_header_bytes) / 4;
    size_t added = 0;
    for (const auto& value : values) {
        if (value.size() > ceiling - header) return maximum;
        const auto cost = header + value.size();
        if (cost > ceiling - added) return maximum;
        added += cost;
    }
    // Retain more batches for small members, reducing the copy window as
    // batch sizes grow. Large batches still use the format maximum.
    const size_t batches = added <= ceiling / 8 ? 16 :
        added <= ceiling / 4 ? 8 : 4;
    const auto minimum = size_t(2048) - list_page_codec::page_header_bytes;
    const auto aligned = added ? (minimum + added - 1) / added : 0;
    auto slots = std::max(aligned, batches);
    if (values.size() == 1) {
        // Keep complete compact pages within a smaller allocation class
        // when dropping only one or two members is enough.
        const auto packed = size_t(4) + values[0].size();
        constexpr size_t tiny_body = 511 - list_page_codec::page_header_bytes;
        constexpr size_t small_body = 1023 - list_page_codec::page_header_bytes;
        if (packed * slots > tiny_body &&
            packed * (slots - slots / 8) <= tiny_body)
            slots = tiny_body / packed;
        else if (slots >= 10 && packed * slots > small_body &&
            packed * (slots - 2) <= small_body)
            slots -= packed * (slots - 1) <= small_body ? 1 : 2;
        else if (slots > 8 && packed * slots > minimum &&
            packed * (slots - 1) <= minimum) --slots;
        else if (slots >= 8) {
            // A full compact page needs one extra byte for the terminator.
            constexpr size_t page_body = 4095 - list_page_codec::page_header_bytes;
            if (packed * slots > page_body && packed * (slots - 1) <= page_body)
                --slots;
        }
    }
    return list_page_codec::page_header_bytes + slots * added;
}

} // namespace

list_store::list_store(page_transaction& transaction, const std::string& owner)
: transaction_(transaction)
, owner_(owner)
, entry_(transaction.roots().find(owner))
{}

list_store::~list_store() {
    if (dirty_) transaction_.abort();
}

bool list_store::fail(status value) {
    if (error_ == status::ok) error_ = value;
    // A nonpaged format is a probe that an authorized importer may replace.
    if (value != status::not_paged) transaction_.abort();
    return false;
}

bool list_store::open() {
    if (!entry_) return fail(status::invalid);
    if ((!entry_->found && entry_->action != db::update_action::write) ||
        entry_->action == db::update_action::erase) {
        root_ = list_root();
        return true;
    }
    if (compact_list_root::matches(entry_->value)) {
        try {
            const compact_list_root::view packed(entry_->value);
            std::vector<std::string> values;
            values.reserve(packed.count());
            for (unsigned i = 0; i < packed.count(); ++i)
                values.push_back(packed.at(i).str());
            initialize(packed.deadline());
            if (error_ != status::ok ||
                !push(list_values_view(values.data(), values.size()), false) ||
                !finish()) return false;
            entry_->maintenance_only = true;
            return true;
        } catch (const std::runtime_error&) { return fail(status::invalid); }
    }
    const auto result = list_page_codec::decode_root(entry_->value, root_);
    if (result == list_page_status::ok) {
        return root_.elements != 0 || fail(status::invalid);
    }
    if (entry_->value.compare(0, 3, "PLR") != 0) {
        return fail(status::not_paged);
    }
    return fail(result == list_page_status::unsupported_version ?
        status::unsupported : status::invalid);
}

void list_store::initialize(int64_t expires_at_ms) {
    if (!entry_) { fail(status::invalid); return; }
    if (!transaction_.reset_pages(owner_)) { fail(status::storage); return; }
    root_ = list_root();
    root_.expires_at_ms = expires_at_ms;
    entry_->clear_pages = true;
    entry_->action = db::update_action::write;
    error_ = status::ok;
    dirty_ = true;
}

bool list_store::inspect(uint64_t id, std::string& scratch,
    db::page_inspector visit, void* context) {
    const auto result = transaction_.inspect_bound(entry_, id, scratch,
        visit, context);
    if (result == db::lookup_status::found) return true;
    return fail(result == db::lookup_status::error ? status::storage :
        status::invalid);
}

bool list_store::read_page(uint64_t id, list_page& out) {
    std::string bytes;
    struct state {
        list_page& page;
        list_page_status decoded;
    } request{out, list_page_status::invalid};
    auto decode = [](void* raw, const std::string& encoded) {
        auto& job = *static_cast<state*>(raw);
        job.decoded = list_page_codec::decode_page(encoded, job.page);
    };
    if (!inspect(id, bytes, decode, &request)) return false;
    const auto decoded = request.decoded;
    if (decoded != list_page_status::ok) {
        return fail(decoded == list_page_status::out_of_memory ?
            status::storage : decoded == list_page_status::unsupported_version ?
                status::unsupported : status::invalid);
    }
    if (out.id != id) return fail(status::invalid);
    if (!check_links(id, out.previous, out.next)) return false;
    for (const auto& fragment : out.fragments) {
        if (fragment.total_bytes > max_member_bytes) {
            return fail(status::invalid);
        }
    }
    return true;
}

bool list_store::check_links(uint64_t id, uint64_t previous, uint64_t next) {
    if ((root_.last_id && (id > root_.last_id ||
        previous > root_.last_id || next > root_.last_id)) ||
        (id == root_.head && previous != 0) ||
        (id == root_.tail && next != 0)) return fail(status::invalid);
    return true;
}

bool list_store::prepare_push(list_values_view values,
    bool left, list_page& page, bool& applied, size_t fill_target) {
    applied = false;
    const auto id = left ? root_.head : root_.tail;
    std::string bytes;
    list_page_update update;
    struct state {
        list_values_view values;
        list_page_update& update;
        std::string& fallback;
        bool left;
        size_t target;
        uint64_t neighbor;
        list_page_status result;
    } request{values, update, bytes, left, fill_target, root_.last_id + 1,
        list_page_status::invalid};
    auto encode = [](void* raw, const std::string& encoded) {
        auto& job = *static_cast<state*>(raw);
        job.result = list_page_codec::push_end(encoded, job.values,
            job.left, max_member_bytes, job.update, job.target, job.neighbor);
        if (job.result == list_page_status::ok && !job.update.applied &&
            !job.update.linked && &encoded != &job.fallback) {
            try {
                job.fallback = encoded;
            } catch (const std::bad_alloc&) {
                job.result = list_page_status::out_of_memory;
            }
        }
    };
    if (!inspect(id, bytes, encode, &request)) return false;
    const auto result = request.result;
    if (result != list_page_status::ok)
        return fail(result == list_page_status::out_of_memory ?
            status::storage : result == list_page_status::unsupported_version ?
                status::unsupported : status::invalid);
    if (update.id != id) return fail(status::invalid);
    if (!check_links(id, update.previous, update.next)) return false;
    if (update.applied) {
        if (!transaction_.put_bound(entry_, id, std::move(update.bytes)))
            return fail(status::storage);
        root_.elements += values.size(); applied = true;
        return true;
    }
    if (update.linked) {
        if (!allocate(page)) return false;
        if (!transaction_.put_bound(entry_, id, std::move(update.bytes)))
            return fail(status::storage);
        if (left) {
            page.next = id;
            root_.head = page.id;
        } else {
            page.previous = id;
            root_.tail = page.id;
        }
        return true;
    }
    const auto decoded = list_page_codec::decode_page(bytes, page);
    if (decoded != list_page_status::ok)
        return fail(decoded == list_page_status::out_of_memory ?
            status::storage : decoded == list_page_status::unsupported_version ?
                status::unsupported : status::invalid);
    return true;
}

bool list_store::prepare_pop(bool left, std::string* out,
    list_page& page, bool& applied, uint64_t count,
    std::vector<std::string>* values, uint64_t removed_neighbor,
    uint32_t* consumed) {
    applied = false;
    const auto id = removed_neighbor ? (left ? page.next : page.previous) :
        (left ? root_.head : root_.tail);
    std::string bytes;
    list_page_update update;
    struct state {
        list_page_update& update;
        list_page& fallback;
        bool left;
        uint64_t count;
        uint64_t removed_neighbor;
        bool allow_whole;
        std::vector<std::string>* values;
        list_page_status result;
    } request{update, page, left, count, removed_neighbor, consumed != nullptr, values,
        list_page_status::invalid};
    auto encode = [](void* raw, const std::string& encoded) {
        auto& job = *static_cast<state*>(raw);
        job.result = job.count == 1 && !job.removed_neighbor ?
            list_page_codec::pop_end(encoded, job.left, max_member_bytes, job.update) :
            list_page_codec::pop_compact_batch(encoded, job.left, job.count,
                max_member_bytes, job.update, job.values, job.removed_neighbor,
                job.allow_whole);
        if (job.result == list_page_status::ok && !job.update.applied &&
            !job.update.consumed)
            job.result = list_page_codec::decode_page(encoded, job.fallback);
    };
    if (!inspect(id, bytes, encode, &request)) return false;
    const auto result = request.result;
    if (result != list_page_status::ok)
        return fail(result == list_page_status::out_of_memory ?
            status::storage : result == list_page_status::unsupported_version ?
                status::unsupported : status::invalid);
    if ((count == 1 && !removed_neighbor) || update.applied || update.consumed) {
        if (update.id != id) return fail(status::invalid);
        if (!check_links(id, update.previous, update.next)) return false;
    }
    if (update.consumed) {
        if (!consumed || update.consumed > root_.elements) return fail(status::invalid);
        page.id = id;
        page.previous = update.previous;
        page.next = update.next;
        *consumed = update.consumed;
        root_.elements -= update.consumed;
        return true;
    }
    if (update.applied) {
        if (root_.elements <= count) return fail(status::invalid);
        if (out) out->swap(update.value);
        if (!transaction_.put_bound(entry_, id, std::move(update.bytes)))
            return fail(status::storage);
        root_.elements -= count; applied = true;
        return true;
    }
    if (count != 1 || removed_neighbor) {
        if (page.id != id) return fail(status::invalid);
        if (!check_links(id, page.previous, page.next)) return false;
        for (const auto& fragment : page.fragments)
            if (fragment.total_bytes > max_member_bytes)
                return fail(status::invalid);
    }
    return true;
}

bool list_store::write_page(const list_page& page) {
    std::string bytes;
    const auto encoded = list_page_codec::encode_packed_page(page, bytes);
    if (encoded != list_page_status::ok) {
        return fail(encoded == list_page_status::out_of_memory ?
            status::storage : status::invalid);
    }
    if (!transaction_.put_bound(entry_, page.id, std::move(bytes))) {
        return fail(status::storage);
    }
    return true;
}

size_t list_store::used(const list_page& page) {
    size_t bytes = list_page_codec::page_header_bytes;
    for (const auto& fragment : page.fragments) {
        bytes += list_page_codec::fragment_header_bytes + fragment.bytes.size();
    }
    return bytes;
}

bool list_store::counter() {
    if (root_.last_id || !root_.elements) return true;
    // One-time promotion of roots created before the id counter was added.
    uint64_t id = root_.head, previous = 0, maximum = 0;
    std::unordered_set<uint64_t> visited;
    while (id) {
        list_page page;
        if (!visited.insert(id).second || !read_page(id, page) ||
            page.previous != previous) return fail(status::invalid);
        maximum = std::max(maximum, id);
        previous = id;
        id = page.next;
    }
    if (previous != root_.tail) return fail(status::invalid);
    root_.last_id = maximum;
    return true;
}

bool list_store::allocate(list_page& page) {
    if (root_.last_id == UINT64_MAX) return fail(status::too_large);
    page = list_page();
    page.id = ++root_.last_id;
    return true;
}

bool list_store::grow(list_page& page, bool left) {
    list_page next;
    if (!allocate(next)) return false;
    if (page.id) {
        if (left) {
            page.previous = next.id;
            next.next = page.id;
        } else {
            page.next = next.id;
            next.previous = page.id;
        }
        if (!write_page(page)) return false;
    } else {
        root_.head = root_.tail = next.id;
    }
    page = std::move(next);
    if (left) root_.head = page.id;
    else root_.tail = page.id;
    return true;
}

bool list_store::append(list_page& page, const std::string& value, bool left,
    size_t fill_target) {
    uint64_t offset = left ? value.size() : 0;
    bool first = true;
    while (first || (left ? offset != 0 : offset < value.size())) {
        first = false;
        const auto current = used(page);
        const auto limit = current > fill_target ?
            list_page_codec::max_page_bytes : fill_target;
        const size_t available = limit - current;
        const size_t header = list_page_codec::fragment_header_bytes;
        if (!page.id || available < header + (value.empty() ? 0 : 1)) {
            if (!grow(page, left)) return false;
            first = true;
            continue;
        }
        const size_t remaining = left ? static_cast<size_t>(offset) :
            value.size() - static_cast<size_t>(offset);
        // Keep small members whole without wasting most of a fresh page.
        // A split at this boundary forces both pages into fragment format.
        if (remaining == value.size() && !page.fragments.empty() &&
            remaining > available - header &&
            remaining <= (fill_target -
                list_page_codec::page_header_bytes) / 4 - header) {
            if (!grow(page, left)) return false;
            first = true;
            continue;
        }
        const auto count = std::min(remaining, available - header);
        list_fragment fragment;
        fragment.total_bytes = value.size();
        fragment.offset = left ? offset - count : offset;
        fragment.bytes.assign(value, static_cast<size_t>(fragment.offset),
            count);
        if (left) {
            page.fragments.insert(page.fragments.begin(), std::move(fragment));
            offset -= count;
        } else {
            page.fragments.push_back(std::move(fragment));
            offset += count;
        }
    }
    return true;
}

bool list_store::push(list_values_view values, bool left) {
    if (error_ != status::ok) return false;
    if (values.size() > static_cast<uint64_t>(LLONG_MAX) - root_.elements) {
        return fail(status::too_large);
    }
    for (const auto& value : values) {
        if (value.size() > max_member_bytes) return fail(status::too_large);
    }
    if (values.empty()) return true;
    dirty_ = true;
    if (!root_.elements) {
        if (!transaction_.reset_pages(owner_)) return fail(status::storage);
        entry_->action = db::update_action::write;
    }
    if (!counter()) return false;
    const auto fill_target = push_target(values);
    list_page page;
    if (root_.elements) {
        bool applied;
        if (!prepare_push(values, left, page, applied, fill_target))
            return false;
        if (applied) return true;
    }
    if (values.size() == 1 && page.fragments.empty() && values[0].size() <=
        fill_target - list_page_codec::page_header_bytes -
            list_page_codec::fragment_header_bytes) {
        if (!page.id && !grow(page, left)) return false;
        std::string bytes;
        const auto encoded = list_page_codec::encode_single_member(page,
            values[0], bytes);
        if (encoded != list_page_status::ok)
            return fail(encoded == list_page_status::out_of_memory ?
                status::storage : status::invalid);
        if (!transaction_.put_bound(entry_, page.id, std::move(bytes)))
            return fail(status::storage);
        ++root_.elements;
        return true;
    }
    for (const auto& value : values) {
        if (!append(page, value, left, fill_target)) return false;
        ++root_.elements;
    }
    if (!write_page(page)) return false;
    dirty_ = true;
    return true;
}

bool list_store::advance(list_page& page, bool left) {
    if (!page.fragments.empty()) return true;
    const auto previous = page.id;
    const auto next = left ? page.next : page.previous;
    if (!transaction_.erase_bound(entry_, previous))
        return fail(status::storage);
    if (!next) {
        page = list_page();
        root_.head = root_.tail = 0;
        return true;
    }
    // Page reads see staged erasures, so a link to any consumed page fails.
    if (!read_page(next, page) ||
        (left ? page.previous : page.next) != previous) {
        return fail(status::invalid);
    }
    if (left) {
        page.previous = 0;
        root_.head = next;
    } else {
        page.next = 0;
        root_.tail = next;
    }
    return true;
}

bool list_store::take(list_page& page, bool left, std::string* out) {
    if (!advance(page, left) || !page.id) return fail(status::invalid);
    auto& first = left ? page.fragments.front() : page.fragments.back();
    const auto total = first.total_bytes;
    if (first.offset == 0 && first.bytes.size() == total) {
        if (out) *out = std::move(first.bytes);
        if (left) page.fragments.erase(page.fragments.begin());
        else page.fragments.pop_back();
        return true;
    }
    if (left ? first.offset != 0 :
        first.offset + first.bytes.size() != total) return
            fail(status::invalid);
    uint64_t offset = left ? 0 : total;
    if (out) out->assign(static_cast<size_t>(total), '\0');
    bool more;
    do {
        if (!advance(page, left) || !page.id) {
            return fail(status::invalid);
        }
        const auto& fragment = left ? page.fragments.front() :
            page.fragments.back();
        if (fragment.total_bytes != total || (left ? fragment.offset != offset :
            fragment.offset + fragment.bytes.size() != offset)) {
            return fail(status::invalid);
        }
        if (out && !fragment.bytes.empty()) {
            std::memcpy(&(*out)[static_cast<size_t>(fragment.offset)],
                fragment.bytes.data(), fragment.bytes.size());
        }
        if (left) {
            offset += fragment.bytes.size();
            page.fragments.erase(page.fragments.begin());
        } else {
            offset = fragment.offset;
            page.fragments.pop_back();
        }
        more = left ? offset != total : offset != 0;
    } while (more);
    return true;
}

bool list_store::pop(uint64_t count, bool left,
    std::vector<std::string>* out) {
    if (out) out->clear();
    return pop_values(count, left, out, nullptr);
}

bool list_store::pop_one(bool left, std::string& out) {
    out.clear();
    return pop_values(1, left, nullptr, &out);
}

bool list_store::pop_values(uint64_t count, bool left,
    std::vector<std::string>* out, std::string* single) {
    if (error_ != status::ok) return false;
    const auto take_count = std::min(count, root_.elements);
    if (!take_count) return true;
    dirty_ = true;
    list_page page;
    uint32_t first_consumed = 0;
    if (take_count == 1) {
        std::string value;
        bool applied;
        if (!prepare_pop(left, out ? &value : single, page, applied))
            return false;
        if (applied) {
            if (out) out->push_back(std::move(value));
            return true;
        }
    } else {
        bool applied;
        if (!prepare_pop(left, nullptr, page, applied, take_count, out,
                0, &first_consumed)) return false;
        if (applied) return true;
    }
    if (out && take_count <= 128) out->reserve(static_cast<size_t>(take_count));
    for (uint64_t remaining = take_count - first_consumed; remaining;) {
        // A compact neighbor can absorb the remaining pop and the endpoint
        // link change in one rewrite, without decoding its surviving members.
        if (page.fragments.empty() && (left ? page.next : page.previous)) {
            const auto removed = page.id;
            const auto next = left ? page.next : page.previous;
            if (!transaction_.erase_bound(entry_, removed)) return fail(status::storage);
            bool applied;
            uint32_t consumed = 0;
            const auto prefix = out ? out->size() : 0;
            if (!prepare_pop(left, nullptr, page, applied, remaining,
                    out, removed, &consumed)) {
                if (out) out->resize(prefix);
                return false;
            }
            if (left) root_.head = next;
            else root_.tail = next;
            if (applied || consumed) {
                if (applied) return true;
                remaining -= consumed;
                continue;
            }
            auto& link = left ? page.previous : page.next;
            if (link != removed) return fail(status::invalid);
            link = 0;
        }
        if (remaining > 1) {
            if (!advance(page, left) || !page.id)
                return fail(status::invalid);
            auto& fragments = page.fragments;
            const auto limit = std::min<uint64_t>(remaining, fragments.size());
            size_t complete = 0;
            for (; complete < limit; ++complete) {
                auto& item = fragments[left ? complete :
                    fragments.size() - 1 - complete];
                if (item.offset || item.total_bytes != item.bytes.size()) break;
                if (out) out->push_back(std::move(item.bytes));
            }
            if (complete) {
                // Remove one contiguous run. In particular, a left pop no
                // longer shifts the remainder once per complete member.
                if (left) fragments.erase(fragments.begin(),
                    fragments.begin() + complete);
                else fragments.resize(fragments.size() - complete);
                root_.elements -= complete;
                remaining -= complete;
                continue;
            }
        }
        std::string value;
        if (!take(page, left, out ? &value : single)) return false;
        if (out) out->push_back(std::move(value));
        --root_.elements;
        --remaining;
    }
    if (!advance(page, left)) return false;
    if ((root_.elements == 0) != (page.id == 0)) return fail(status::invalid);
    if (page.id && !write_page(page)) return false;
    dirty_ = true;
    return true;
}

bool list_store::set_expiry(int64_t expires_at_ms) {
    if (error_ != status::ok) return false;
    if (expires_at_ms < -1) return fail(status::invalid_argument);
    if (root_.elements && root_.expires_at_ms != expires_at_ms) {
        root_.expires_at_ms = expires_at_ms;
        dirty_ = true;
    }
    return true;
}

bool list_store::finish() {
    if (error_ != status::ok) return false;
    if (!dirty_) return true;
    entry_->maintenance_only = false;
    if (!root_.elements) {
        entry_->action = db::update_action::erase;
        entry_->clear_pages = false;
        dirty_ = false;
        return true;
    }
    const auto encoded = list_page_codec::encode_root(root_, entry_->value);
    if (encoded != list_page_status::ok) return fail(
        encoded == list_page_status::out_of_memory ? status::storage :
            status::invalid);
    entry_->action = db::update_action::write;
    dirty_ = false;
    return true;
}

} // namespace pkv
