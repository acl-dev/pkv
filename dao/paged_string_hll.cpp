#include "stdafx.h"
#include "paged_string.h"
#include "common/redis_hll.h"

namespace pkv {
namespace dao {
bool paged_string::hll_value(string_store& store, std::string& value,
    bool body) {
    if (!store.range(0, 15, value)) return stored(store);
    const auto length = store.metadata().length;
    if (!hll::header(value, length)) {
        read_status_ = read_status::invalid_hll; return false;
    }
    if (!body) return true;
    if (length > hll::maximum_size) {
        read_status_ = read_status::corrupt_hll; return false;
    }
    return store.read(value) || stored(store);
}
bool paged_string::pfadd(shared_db& db, const std::string& key,
    const std::vector<const std::string*>& elements, long long& changed) {
    changed = 0;
    bool updated = false;
    const auto ok = run(db, key, [&](string_store& store) {
        std::string value;
        updated = !store.exists();
        if (updated) value = hll::empty();
        else if (!hll_value(store, value, !elements.empty())) return false;
        for (const auto* element : elements) {
            bool added;
            if (!hll::add(value, *element, added)) {
                read_status_ = read_status::corrupt_hll; return false;
            }
            updated = updated || added;
        }
        if (!updated) return true;
        hll::invalidate(value);
        return store.assign(value) || stored(store);
    });
    if (ok) changed = updated ? 1 : 0;
    return ok;
}
bool paged_string::pfcount(shared_db& db,
    const std::vector<std::string>& keys, long long& count) {
    count = 0;
    if (keys.size() != 1) return pfaggregate(db, keys, false, count);
    uint64_t cardinality = 0;
    const auto ok = run(db, keys.front(), [&](string_store& store) {
        if (!store.exists()) return true;
        std::string value;
        if (!hll_value(store, value, false)) return false;
        if (hll::cached(value, cardinality)) return true;
        if (!hll_value(store, value, true)) return false;
        hll::registers registers{};
        if (!hll::merge(value, registers)) {
            read_status_ = read_status::corrupt_hll; return false;
        }
        cardinality = hll::count(registers); hll::cache(value, cardinality);
        store.touch();
        return store.write(8, value.substr(8, 8)) || stored(store);
    });
    if (ok) count = static_cast<long long>(cardinality);
    return ok;
}
bool paged_string::pfmerge(shared_db& db,
    const std::vector<std::string>& keys) {
    long long ignored;
    return pfaggregate(db, keys, true, ignored);
}
} // namespace dao
} // namespace pkv
