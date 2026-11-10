#include "stdafx.h"
#include "paged_string.h"
#include "string_bitop_source.h"
#include "common/redis_hll.h"

namespace pkv {
namespace dao {
namespace {
bool merge_source(shared_db& db, page_transaction& tx, const std::string& key,
    int64_t now, std::string& scratch, hll::registers& maximum, bool& dense,
    std::string* destination_header, dao_base::read_status& error) {
    using status = dao_base::read_status;
    string_bitop_source input(db, tx, key, now);
    if (!input.open()) { error = input.error(); return false; }
    if (!input.exists()) {
        if (input.expired())
            tx.roots().find(key)->action = db::update_action::erase;
        return true;
    }
    if (!input.length()) { error = status::invalid_hll; return false; }
    struct merge_input {
        hll::registers& maximum;
        bool& dense;
        std::string* destination_header;
        status failure;
    } work{maximum, dense, destination_header, status::ok};
    const auto merge = [](void* raw, const std::string& bytes) {
        auto& data = *static_cast<merge_input*>(raw);
        if (!hll::header(bytes, bytes.size())) {
            data.failure = status::invalid_hll; return;
        }
        if (!hll::merge(bytes, data.maximum)) {
            data.failure = status::corrupt_hll; return;
        }
        data.dense = data.dense || bytes[4] == 0;
        if (data.destination_header)
            data.destination_header->assign(bytes.data(), hll::header_size);
    };
    if (input.length() <= string_page_codec::page_bytes) {
        if (!input.inspect_value(scratch, merge, &work)) {
            error = input.error(); return false;
        }
    } else {
        if (!input.read(0, hll::header_size, scratch)) {
            error = input.error(); return false;
        }
        if (!hll::header(scratch, input.length())) {
            error = status::invalid_hll; return false;
        }
        if (input.length() > hll::maximum_size) {
            error = status::corrupt_hll; return false;
        }
        if (!input.read(0, static_cast<size_t>(input.length()), scratch)) {
            error = input.error(); return false;
        }
        merge(&work, scratch);
    }
    if (work.failure == status::ok) return true;
    error = work.failure;
    return false;
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
        std::string value, destination_header;
        bool dense = false;
        const auto now = unix_time_ms();
        for (size_t i = 0; i < keys.size(); ++i) {
            if (!merge_source(db, tx, keys[i], now, value, maximum, dense,
                write && !i ? &destination_header : nullptr, read_status_))
                return false;
        }
        if (!write) { estimate = hll::count(maximum); return true; }
        string_store target(tx, keys.front());
        now_ = now;
        if (!open(db, tx, keys.front(), target)) return false;
        if (dense) {
            if (destination_header.empty()) destination_header = hll::empty();
            hll::pack_dense(destination_header, maximum);
            hll::invalidate(destination_header);
            return (target.assign_page(std::move(destination_header)) &&
                target.finish()) || stored(target);
        }
        if (target.exists()) {
            if (!hll_value(target, value, true)) return false;
        } else value = hll::empty();
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
