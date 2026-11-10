#include "stdafx.h"
#include "paged_zset.h"
#include <algorithm>
#include <cmath>

namespace pkv {
namespace dao {
namespace {
bool better(const geo::match& first, const geo::match& second, int order) {
    return order > 0 ? first.distance < second.distance :
        first.distance > second.distance;
}

template<class Visitor>
void visit_compact(const compact_zset::view& packed, const geo::score_range& range,
    const Visitor& visit) {
    unsigned first = 0, last = packed.count();
    while (first < last) {
        const auto mid = first + (last - first) / 2;
        const auto item = packed.at(packed.ordered_index(mid));
        if (packed.score(item.second) < range.lower) first = mid + 1;
        else last = mid;
    }
    for (unsigned i = first; i < packed.count(); ++i) {
        const auto item = packed.at(packed.ordered_index(i));
        const auto score = packed.score(item.second);
        if (score >= range.upper || !visit(item.first, score)) break;
    }
}

bool geo_collect(zset_store* store, const compact_zset::view* packed,
    const geo::search_options& options, std::vector<geo::match>& out) {
    const auto count = packed ? packed->count() : store->metadata().elements;
    if (!count) return true;
    const bool limited = options.count && !options.any && options.order &&
        static_cast<uint64_t>(options.count) < count;
    const auto compare = [&](const geo::match& a, const geo::match& b) {
        return better(a, b, options.order);
    };
    const auto collect = [&](compact_zset::member_view member, double score) {
        geo::match value;
        value.position = geo::decode(score);
        if (!geo::contains(options.area, value.position, value.distance))
            return true;
        value.member.assign(member.data(), member.size()); value.score = score;
        if (limited) {
            const auto limit = static_cast<uint64_t>(options.count);
            if (out.size() < limit) {
                out.push_back(std::move(value));
                std::push_heap(out.begin(), out.end(), compare);
            } else if (compare(value, out.front())) {
                std::pop_heap(out.begin(), out.end(), compare);
                out.back() = std::move(value);
                std::push_heap(out.begin(), out.end(), compare);
            }
        } else out.push_back(std::move(value));
        return !options.any || !options.count || out.size() <
            static_cast<uint64_t>(options.count);
    };
    auto ranges = geo::search_ranges(options.area);
    size_t intervals = 0;
    // Only join forward neighbors in encounter order. This preserves both
    // COUNT ANY's stopping point and the order of equal-distance matches.
    for (auto range : ranges) {
        if (!range.enabled) continue;
        if (intervals && ranges[intervals - 1].upper == range.lower)
            ranges[intervals - 1].upper = range.upper;
        else ranges[intervals++] = range;
    }
    for (size_t i = 0; i < intervals; ++i) {
        const auto& range = ranges[i];
        if (options.any && options.count && out.size() >=
            static_cast<uint64_t>(options.count)) break;
        if (packed) {
            visit_compact(*packed, range, collect);
        } else {
            zset_query query; query.mode = zrange_mode::score;
            query.min_score.value = range.lower;
            query.max_score.value = range.upper; query.max_score.exclusive = true;
            if (!store->visit_range(query, collect, !options.any)) return false;
        }
    }
    if (options.order) std::stable_sort(out.begin(), out.end(), compare);
    return true;
}
} // namespace

bool paged_zset::geo_search(shared_db& db, const std::string& source,
    const std::string* destination, const geo_parser& configure,
    geo::search_options& options, std::vector<geo::match>& out,
    long long& stored) {
    reset(); out.clear(); stored = 0; options = geo::search_options();
    try {
        const auto prepare = [&](const geo::search_source& input) {
            if (!configure(input, options)) return false;
            const auto& area = options.area;
            if (options.count < 0 || options.order < -1 ||
                options.order > 1 || !geo::valid(area.center) ||
                !std::isfinite(area.conversion) || area.conversion <= 0 ||
                std::isnan(area.radius) || area.radius < 0 ||
                std::isnan(area.width) || area.width < 0 ||
                std::isnan(area.height) || area.height < 0) {
                read_status_ = read_status::invalid_record; return false;
            }
            if (options.count && !options.order && !options.any) options.order = 1;
            return true;
        };
        const auto apply = [&](page_transaction& tx) {
            const auto now = unix_time_ms();
            auto& root = *tx.roots().find(source);
            if (root.found && compact_zset::matches(root.value)) {
                try {
                    const compact_zset::view packed(root.value);
                    packed.validate_fast();
                    const auto deadline = packed.deadline();
                    const bool live = deadline < 0 || deadline > now;
                    if (!live) root.action = db::update_action::erase;
                    const geo::search_source input{live, [&](const std::string& key,
                        zset_score& score) {
                        score = zset_score();
                        score.found = live && packed.find(key, score.score);
                        return true;
                    }};
                    if (!prepare(input) ||
                        (live && !geo_collect(nullptr, &packed, options, out))) return false;
                } catch (const std::runtime_error&) {
                    read_status_ = read_status::invalid_record;
                    return false;
                }
            } else {
                zset_store from(tx, source);
                if (!open(db, tx, source, from, now)) return false;
                const geo::search_source input{from.exists(), [&](const std::string& key,
                    zset_score& score) { return from.get(key, score); }};
                if (!prepare(input) || !geo_collect(&from, nullptr, options, out)) {
                    if (from.error() != zset_store::status::ok)
                        read_status_ = map_read_status(from.error());
                    return false;
                }
                if (!finish(from)) return false;
            }
            if (!destination) return true;
            const auto score = [&](const geo::match& value) {
                return options.storedist ? value.distance / options.area.conversion : value.score;
            };
            stored = static_cast<long long>(out.size());
            if (db->supports_compact_zsets() && !out.empty() && out.size() <= compact_zset::max_members) {
                std::vector<compact_zset::entry> items;
                items.reserve(out.size());
                for (const auto& value : out) items.emplace_back(value.member, score(value));
                bool compact;
                if (!store_compact(tx, *destination, std::move(items), compact)) return false;
                if (compact) return true;
            }
            zset_store target(tx, *destination);
            target.initialize();
            for (const auto& value : out) {
                bool added, changed;
                if (!target.put(value.member, score(value), added, changed))
                    return finish(target);
            }
            return finish(target);
        };
        const bool ok = result(destination ? db->transact_pages({source, *destination}, apply) :
            db->transact_page(source, apply));
        if (!ok) { out.clear(); stored = 0; }
        return ok;
    } catch (const std::bad_alloc&) {
        out.clear(); stored = 0;
        read_status_ = read_status::storage_error; return false;
    }
}
} // namespace dao
} // namespace pkv
