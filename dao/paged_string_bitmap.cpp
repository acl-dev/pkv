#include "stdafx.h"
#include "paged_string.h"

namespace pkv {
namespace dao {

bool paged_string::valid_bit_offset(long long offset) {
    if (offset >= 0 && static_cast<uint64_t>(offset) <
        string_page_codec::write_bytes * 8) return true;
    reset(); read_status_ = read_status::bit_offset;
    return false;
}

bool paged_string::getbit(shared_db& db, const std::string& key,
    long long offset, long long& bit) {
    bit = 0;
    if (!valid_bit_offset(offset)) return false;
    const bool ok = run(db, key, [&](string_store& store) {
        return store.getbit(static_cast<uint64_t>(offset), bit);
    });
    if (!ok) bit = 0;
    return ok;
}

bool paged_string::setbit(shared_db& db, const std::string& key,
    long long offset, int bit, long long& previous) {
    previous = 0;
    if (!valid_bit_offset(offset)) return false;
    if (bit != 0 && bit != 1) {
        reset(); read_status_ = read_status::invalid_bit; return false;
    }
    const bool ok = run(db, key, [&](string_store& store) {
        return store.setbit(static_cast<uint64_t>(offset), bit != 0, previous);
    });
    if (!ok) previous = 0;
    return ok;
}

bool paged_string::bitcount(shared_db& db, const std::string& key,
    const bitmap_range& range, long long& count) {
    count = 0;
    const bool ok = run(db, key, [&](string_store& store) {
        return store.bitcount(range, count);
    });
    if (!ok) count = 0;
    return ok;
}

bool paged_string::bitpos(shared_db& db, const std::string& key, int bit,
    const bitmap_range& range, long long& position) {
    position = -1;
    if (bit != 0 && bit != 1) {
        reset(); read_status_ = read_status::invalid_bit; return false;
    }
    const bool ok = run(db, key, [&](string_store& store) {
        return store.bitpos(bit != 0, range, position);
    });
    if (!ok) position = -1;
    return ok;
}

} // namespace dao
} // namespace pkv
