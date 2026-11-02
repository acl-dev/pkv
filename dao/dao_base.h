#pragma once
#include "db/db.h"
#include "coder/record_codec.h"

namespace pkv {
namespace dao {

class dao_base {
public:
    dao_base() = default;
    virtual ~dao_base() = default;
    enum class read_status {
        ok, missing, wrong_type, invalid_record, storage_error,
        unsupported_format, unsupported_operation, invalid_integer,
        invalid_hash_integer, integer_overflow, decrement_overflow,
        offset_range, string_too_large, invalid_expiry, no_key, index_range,
        list_count_range, sample_too_large, unsupported_scan, same_object,
        invalid_score, nan_score, invalid_hash_float, nonfinite_increment,
        nonfinite_result, bit_offset, invalid_bit, readonly_bitfield,
        invalid_digest, delex_type, delex_condition, invalid_hll, corrupt_hll
    };
    read_status status() const { return read_status_; }
    const char* error() const;
    static bool save(shared_db& db, const std::string& key, const std::string& data);
    bool read(shared_db& db, const std::string& key);
    void reset() { view_.reset(); read_status_ = read_status::missing; }
protected:
    bool decode(shared_db& db, const std::string& bytes, long long now_ms = -1);
    bool materialize(value_record& record);
    bool encode(shared_db& db, const value_record& record, std::string& bytes);
    read_status read_status_ = read_status::missing;
    std::unique_ptr<record_view> view_;
};

} // namespace dao
} // namespace pkv
