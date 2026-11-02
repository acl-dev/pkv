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
}
bool paged_zset::geo_collect(zset_store& store,
    const geo::search_options& options, std::vector<geo::match>& out) {
    if (!store.exists()) return true;
    const bool limited = options.count && !options.any && options.order &&
        static_cast<uint64_t>(options.count) < store.metadata().elements;
    const auto compare = [&](const geo::match& a, const geo::match& b) {
        return better(a, b, options.order);
    };
    for (const auto& range : geo::search_ranges(options.area)) {
        if (!range.enabled) continue;
        if (options.any && options.count && out.size() >=
            static_cast<uint64_t>(options.count)) break;
        zset_query query; query.mode = zrange_mode::score;
        query.min_score.value = range.lower;
        query.max_score.value = range.upper; query.max_score.exclusive = true;
        if (!store.visit_range(query, [&](const std::string& member,
            double score) {
            geo::match value;
            value.position = geo::decode(score);
            if (!geo::contains(options.area, value.position, value.distance))
                return true;
            value.member = member; value.score = score;
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
        })) {
            read_status_ = map_read_status(store.error()); return false;
        }
    }
    if (options.order) std::stable_sort(out.begin(), out.end(), compare);
    return true;
}
bool paged_zset::geo_search(shared_db& db, const std::string& source,
    const std::string* destination, const geo_parser& configure,
    geo::search_options& options, std::vector<geo::match>& out,
    long long& stored) {
    reset(); out.clear(); stored = 0; options = geo::search_options();
    try {
        std::vector<std::string> keys{source};
        if (destination) keys.push_back(*destination);
        const bool ok = result(db->transact_pages(std::move(keys),
            [&](page_transaction& tx) {
            {
                zset_store from(tx, source);
                if (!open(db, tx, source, from, unix_time_ms())) return false;
                if (!configure(from, options)) {
                    if (from.error() != zset_store::status::ok)
                        read_status_ = map_read_status(from.error());
                    return false;
                }
                const auto& area = options.area;
                if (options.count < 0 || options.order < -1 ||
                    options.order > 1 ||
                    !geo::valid(area.center) ||
                    !std::isfinite(area.conversion) ||
                    area.conversion <= 0 || std::isnan(area.radius) ||
                    area.radius < 0 || std::isnan(area.width) ||
                    area.width < 0 ||
                    std::isnan(area.height) || area.height < 0) {
                    read_status_ = read_status::invalid_record; return false;
                }
                if (options.count && !options.order && !options.any)
                    options.order = 1;
                if (!geo_collect(from, options, out) || !finish(from))
                    return false;
            }
            if (!destination) return true;
            zset_store target(tx, *destination);
            target.initialize();
            for (const auto& value : out) {
                bool added, changed;
                const auto score = options.storedist ?
                    value.distance / options.area.conversion : value.score;
                if (!target.put(value.member, score, added, changed))
                    return finish(target);
            }
            stored = static_cast<long long>(out.size());
            return finish(target);
        }));
        if (!ok) { out.clear(); stored = 0; }
        return ok;
    } catch (const std::bad_alloc&) {
        out.clear(); stored = 0;
        read_status_ = read_status::storage_error; return false;
    }
}
} // namespace dao
} // namespace pkv
