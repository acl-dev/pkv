#include "map_store.h"
#include <algorithm>
#include <iterator>

namespace pkv {

bool map_store::set_bucket_at_rank(uint64_t& rank, unsigned& bucket,
    const set_pop_groups* pop) {
    for (unsigned group = 0; group < 16; ++group) {
        const auto count = index_->top.buckets[group].elements;
        if (rank >= count) { rank -= count; continue; }
        if (!read_group(group, pop)) return false;
        for (unsigned i = 0; i < 16; ++i) {
            const auto amount = index_->group.buckets[i].elements;
            if (rank < amount) {
                bucket = group * 16 + i;
                return true;
            }
            rank -= amount;
        }
        break;
    }
    return fail(status::invalid);
}

bool map_store::bucket_select(uint64_t rank, std::string& key, bool& found,
    std::string* value) {
    found = false;
    if (kind_ == map_kind::set && root_.grouped) {
        if (rank >= root_.elements) return true;
        unsigned bucket = 0;
        if (!read_directory(false) || !set_bucket_at_rank(rank, bucket)) return false;
        auto apply = [&](map_store& child) { return child.select(rank, key, found, value); };
        return bucket_run(bucket, std::ref(apply), false);
    }
    if (!read_directory()) return false;
    if (rank >= root_.elements) return true;
    for (unsigned i = 0; i < 256; ++i) {
        const auto count = index_->directory->buckets[i].elements;
        if (rank >= count) { rank -= count; continue; }
        auto apply = [&](map_store& child) {
            return child.select(rank, key, found, value);
        };
        return bucket_run(i, std::ref(apply), false);
    }
    return fail(status::invalid);
}

bool map_store::bucket_rank(const std::string& key, uint64_t& rank,
    bool& found) {
    rank = 0; found = false;
    if (!read_directory()) return false;
    const unsigned which = bucket(key);
    uint64_t prefix = 0, local = 0;
    for (unsigned i = 0; i < which; ++i)
        prefix += index_->directory->buckets[i].elements;
    auto apply = [&](map_store& child) {
        return child.rank(key, local, found);
    };
    if (!bucket_run(which, std::ref(apply), false)) return false;
    if (found) rank = prefix + local;
    return true;
}

bool map_store::bucket_bound(const std::string& key, bool upper,
    uint64_t& rank) {
    rank = 0;
    if (!read_directory()) return false;
    const unsigned which = bucket(key);
    uint64_t local = 0;
    auto apply = [&](map_store& child) {
        return child.bound_rank(key, upper, local);
    };
    if (!bucket_run(which, std::ref(apply), false)) return false;
    for (unsigned i = 0; i < which; ++i)
        rank += index_->directory->buckets[i].elements;
    rank += local;
    return true;
}

bool map_store::bucket_visit(const visitor& callback, bool with_values,
    bool lazy_groups) {
    if (!read_directory(!lazy_groups)) return false;
    bool more = true;
    auto emit = [&](const std::string& key, const std::string& value) {
        more = callback(key, value); return more;
    };
    auto apply = [&](map_store& child) {
        return child.visit(std::ref(emit), with_values);
    };
    for (unsigned i = 0; i < 256 && more; ++i) {
        if (lazy_groups) {
            if (!index_->top.buckets[i / 16].elements) continue;
            if (!read_group(i / 16)) return false;
            if (!index_->group.buckets[i % 16].elements) continue;
        } else if (!index_->directory->buckets[i].elements) continue;
        if (!bucket_run(i, std::ref(apply), false)) return false;
    }
    return true;
}

bool map_store::visit_set_prefix(const visitor& callback) {
    if (kind_ != map_kind::set) return fail(status::wrong_kind);
    return root_.grouped ? bucket_visit(callback, false, true) :
        visit(callback, false);
}

bool map_store::bucket_scan(const scan_options& options, scan_page& out) {
    scan_page page;
    if (options.cursor >= 256) { out = std::move(page); return true; }
    if (!read_directory()) return false;
    uint64_t visited = 0;
    unsigned previous = static_cast<unsigned>(options.cursor);
    for (unsigned i = previous; i < 256; ++i) {
        const auto count = index_->directory->buckets[i].elements;
        if (!count) continue;
        if (i != previous && visited >= options.count) {
            page.cursor = previous + 1; out = std::move(page); return true;
        }
        previous = i;
        scan_page part;
        auto request = options; request.cursor = i;
        auto apply = [&](map_store& child) {
            return child.scan(request, part);
        };
        if (!bucket_run(i, std::ref(apply), false)) return false;
        if (part.cursor) return fail(status::invalid);
        visited += count;
        page.values.insert(page.values.end(),
            std::make_move_iterator(part.values.begin()),
            std::make_move_iterator(part.values.end()));
    }
    out = std::move(page);
    return true;
}

bool map_store::bucket_range(uint64_t first, uint64_t count, bool reverse,
    const visitor& callback, bool with_values) {
    if (!count || first >= root_.elements) return true;
    if (!read_directory()) return false;
    count = std::min(count, reverse ? first + 1 : root_.elements - first);
    int index = 0;
    while (index < 256 && first >= index_->directory->buckets[index].elements) {
        first -= index_->directory->buckets[index].elements; ++index;
    }
    bool more = true;
    auto emit = [&](const std::string& key, const std::string& value) {
        more = callback(key, value); return more;
    };
    for (; index >= 0 && index < 256 && count && more;
        index += reverse ? -1 : 1) {
        const auto available = index_->directory->buckets[index].elements;
        if (!available) continue;
        const uint64_t amount = std::min(count, reverse ? first + 1 :
            available - first);
        auto apply = [&](map_store& child) {
            return child.visit_rank(first, amount, reverse,
                std::ref(emit), with_values);
        };
        if (!bucket_run(static_cast<unsigned>(index), std::ref(apply), false))
            return false;
        count -= amount;
        first = reverse ? UINT64_MAX : 0;
        if (reverse && index > 0) {
            int next = index - 1;
            while (next >= 0 && !index_->directory->buckets[next].elements)
                --next;
            if (next >= 0)
                first = index_->directory->buckets[next].elements - 1;
        }
    }
    return !count || !more || fail(status::invalid);
}

} // namespace pkv
