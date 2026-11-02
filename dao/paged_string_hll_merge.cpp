#include "stdafx.h"
#include "paged_string.h"
#include "string_bitop_source.h"
#include "common/redis_hll.h"

namespace pkv {
namespace dao {
namespace {
bool source(shared_db& db, page_transaction& tx, const std::string& key,
    int64_t now, std::string& value, dao_base::read_status& error) {
    string_bitop_source input(db, tx, key, now);
    if (!input.open()) { error = input.error(); return false; }
    value.clear();
    if (!input.exists()) {
        if (input.expired())
            tx.roots().find(key)->action = db::update_action::erase;
        return true;
    }
    if (!input.read(0, 16, value)) { error = input.error(); return false; }
    if (!hll::header(value, input.length())) {
        error = dao_base::read_status::invalid_hll; return false;
    }
    if (input.length() > hll::maximum_size) {
        error = dao_base::read_status::corrupt_hll; return false;
    }
    if (!input.read(0, static_cast<size_t>(input.length()), value)) {
        error = input.error(); return false;
    }
    return true;
}
} // namespace
bool paged_string::pfaggregate(shared_db& db,
    const std::vector<std::string>& keys, bool write, long long& cardinality) {
    reset(); cardinality = 0;
    if (keys.empty()) {
        read_status_ = read_status::invalid_record; return false;
    }
    uint64_t estimate = 0;
    const auto status = db->transact_pages(keys, [&](page_transaction& tx) {
        hll::registers maximum{};
        std::string value;
        bool dense = false;
        const auto now = unix_time_ms();
        for (const auto& key : keys) {
            if (!source(db, tx, key, now, value, read_status_)) return false;
            if (value.empty()) continue;
            dense = dense || value[4] == 0;
            if (!hll::merge(value, maximum)) {
                read_status_ = read_status::corrupt_hll; return false;
            }
        }
        if (!write) { estimate = hll::count(maximum); return true; }
        string_store target(tx, keys.front());
        now_ = now;
        if (!open(db, tx, keys.front(), target)) return false;
        if (target.exists()) {
            if (!hll_value(target, value, true)) return false;
        } else value = hll::empty();
        if (dense && !hll::dense(value)) {
            read_status_ = read_status::corrupt_hll; return false;
        }
        for (size_t i = 0; i < hll::registers_size; ++i) {
            if (!maximum[i]) continue;
            if (!value[4]) { hll::put(value, i, maximum[i]); continue; }
            bool changed;
            if (!hll::set(value, i, maximum[i], changed)) {
                read_status_ = read_status::corrupt_hll; return false;
            }
        }
        hll::invalidate(value);
        return (target.assign(value) && target.finish()) || stored(target);
    });
    if (!result(status)) return false;
    cardinality = static_cast<long long>(estimate); return true;
}
} // namespace dao
} // namespace pkv
