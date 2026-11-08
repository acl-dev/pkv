#include "stdafx.h"
#include "dao_base.h"

namespace pkv {
namespace dao {

const char* dao_base::error() const {
    switch (read_status_) {
    case read_status::readonly_bitfield:
        return "ERR BITFIELD_RO only supports the GET subcommand";
    case read_status::bit_offset:
        return "ERR bit offset is not an integer or out of range";
    case read_status::invalid_bit:
        return "ERR bit is not an integer or out of range";
    case read_status::invalid_hash_float:
        return "ERR hash value is not a float";
    case read_status::nonfinite_increment:
        return "ERR value is NaN or Infinity";
    case read_status::nonfinite_result:
        return "ERR increment would produce NaN or Infinity";
    case read_status::invalid_hash_integer: return "ERR hash value is not an integer";
    case read_status::invalid_integer: return "ERR value is not an integer or out of range";
    case read_status::integer_overflow: return "ERR increment or decrement would overflow";
    case read_status::decrement_overflow: return "ERR decrement would overflow";
    case read_status::offset_range: return "ERR offset is out of range";
    case read_status::string_too_large: return "ERR string exceeds maximum allowed size (512MB)";
    case read_status::invalid_expiry: return "ERR invalid expire time in 'getex' command";
    case read_status::invalid_score: return "ERR value is not a valid float";
    case read_status::nan_score: return "ERR resulting score is not a number (NaN)";
    case read_status::unsupported_scan: return "ERR scanning is not supported by this storage backend";
    case read_status::sample_too_large: return "ERR requested sample is too large";
    case read_status::list_count_range:
        return "ERR value is out of range, value must between -9223372036854775807 and 9223372036854775807";
    case read_status::same_object: return "ERR source and destination objects are the same";
    case read_status::no_key: return "ERR no such key";
    case read_status::index_range: return "ERR index out of range";
    case read_status::invalid_digest:
        return "ERR must be exactly 16 hexadecimal characters";
    case read_status::delex_type:
        return "ERR Key should be of string type if conditions are specified";
    case read_status::delex_condition:
        return "ERR Invalid condition. Use IFEQ, IFNE, IFDEQ, or IFDNE";
    case read_status::invalid_hll:
        return "WRONGTYPE Key is not a valid HyperLogLog string value.";
    case read_status::corrupt_hll:
        return "INVALIDOBJ Corrupted HLL object detected";
    case read_status::wrong_type: return "WRONGTYPE Operation against a key holding the wrong kind of value";
    case read_status::invalid_record: return "ERR invalid stored value";
    case read_status::unsupported_operation: return "ERR atomic batch operations are not supported by this storage backend";
    case read_status::unsupported_format: return "ERR unsupported stored format";
    default: return "ERR storage operation failed";
    }
}

bool dao_base::read(shared_db& db, const std::string& key) {
    reset();
    bool found_value = false;
    const bool ok = db->update(key, [&](bool found, std::string& bytes) {
        if (!found) return db::update_action::keep;
        found_value = decode(db, bytes);
        if (read_status_ == read_status::missing) return db::update_action::erase;
        return db::update_action::keep;
    });
    if (!ok) {
        view_.reset();
        read_status_ = read_status::storage_error;
        return false;
    }
    return found_value;
}

bool dao_base::decode(shared_db& db, const std::string& bytes, long long now_ms) {
    const auto status = db->value_codec().decode_stored(bytes, view_);
    if (status != codec_status::ok || !view_) {
        view_.reset();
        read_status_ = status == codec_status::unsupported_version ? read_status::unsupported_format
            : status == codec_status::invalid_record ? read_status::invalid_record : read_status::storage_error;
        return false;
    }
    const long long deadline = view_->header().expires_at_ms;
    if (deadline >= 0 && deadline <= (now_ms < 0 ? unix_time_ms() : now_ms)) {
        view_.reset();
        read_status_ = read_status::missing;
        return false;
    }
    read_status_ = read_status::ok;
    return true;
}

bool dao_base::decode_metadata(shared_db& db, const std::string& bytes,
    record_header& header, bool& native, long long now_ms) {
    const auto status = db->value_codec().open_stored(bytes, header,
        view_, native);
    if (status != codec_status::ok || (!native && !view_)) {
        view_.reset();
        read_status_ = status == codec_status::unsupported_version ?
            read_status::unsupported_format :
            status == codec_status::invalid_record ?
                read_status::invalid_record : read_status::storage_error;
        return false;
    }
    const auto deadline = header.expires_at_ms;
    if (deadline >= 0 && deadline <= (now_ms < 0 ? unix_time_ms() : now_ms)) {
        view_.reset(); read_status_ = read_status::missing;
        return false;
    }
    read_status_ = read_status::ok;
    return true;
}

bool dao_base::materialize(value_record& record) {
    if (!view_) return false;
    record = value_record();
    record.header = view_->header();
    bool ok = false;
    switch (record.header.kind) {
    case value_kind::string: ok = view_->string_value(record.string_value); break;
    case value_kind::hash: ok = view_->hash_values(record.hash_value); break;
    case value_kind::list: ok = view_->list_values(record.list_value); break;
    case value_kind::set: ok = view_->set_values(record.set_value); break;
    case value_kind::zset: ok = view_->zset_values(record.zset_value); break;
    case value_kind::stream: break;
    }
    if (!ok) read_status_ = read_status::invalid_record;
    return ok;
}

bool dao_base::encode(shared_db& db, const value_record& record, std::string& bytes) {
    const auto status = db->value_codec().encode(record, bytes);
    if (status != codec_status::ok) {
        read_status_ = status == codec_status::unsupported_version ? read_status::unsupported_format
            : status == codec_status::invalid_record ? read_status::invalid_record : read_status::storage_error;
        return false;
    }
    read_status_ = read_status::ok;
    return true;
}

bool dao_base::save(shared_db& db, const std::string& key, const std::string& data) {
    return db->set(key, data);
}

} // namespace dao
} // namespace pkv
