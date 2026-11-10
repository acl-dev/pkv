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
    if (!scope_) {
        auto direct = db->read_string_bit(key, static_cast<uint64_t>(offset), bit);
        if (direct == db::inline_status::fallback && static_cast<uint64_t>(offset) <
                string_page_codec::inline_bytes * 8) {
            // Keep the virtual inline hook for backends outside the native path.
            std::string byte;
            direct = db->read_inline_string(key, byte, static_cast<size_t>(offset / 8), 1);
            if (!byte.empty())
                bit = (static_cast<unsigned char>(byte[0]) >> (7 - offset % 8)) & 1;
        }
        if (direct != db::inline_status::fallback) {
            reset();
            if (direct == db::inline_status::error) {
                read_status_ = read_status::storage_error;
                return false;
            }
            read_status_ = read_status::ok;
            return true;
        }
    }
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
    if (!scope_) {
        struct bit_edit {
            unsigned char mask;
            int bit;
            long long& previous;
        } request{static_cast<unsigned char>(1U << (7 - offset % 8)), bit, previous};
        const auto update = [](void* raw, char* bytes) {
            auto& edit = *static_cast<bit_edit*>(raw);
            const auto old = static_cast<unsigned char>(*bytes);
            edit.previous = (old & edit.mask) != 0;
            if (edit.previous == edit.bit) return false;
            *bytes = static_cast<char>(old ^ edit.mask);
            return true;
        };
        const auto direct = db->edit_string_bytes(key, offset / 8, 1, update, &request);
        if (direct != db::inline_status::fallback) {
            reset();
            if (direct == db::inline_status::found) {
                read_status_ = read_status::ok;
                return true;
            }
            previous = 0;
            read_status_ = read_status::storage_error;
            return false;
        }
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
    if (!scope_) {
        const auto direct = db->scan_string_bitmap(key, range, -1, count);
        if (direct != db::inline_status::fallback) {
            reset();
            const bool ok = direct != db::inline_status::error;
            read_status_ = ok ? read_status::ok : read_status::storage_error;
            return ok;
        }
    }
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
    if (!scope_) {
        const auto direct = db->scan_string_bitmap(key, range, bit, position);
        if (direct != db::inline_status::fallback) {
            reset();
            if (direct == db::inline_status::missing && !bit) position = 0;
            const bool ok = direct != db::inline_status::error;
            read_status_ = ok ? read_status::ok : read_status::storage_error;
            return ok;
        }
    }
    const bool ok = run(db, key, [&](string_store& store) {
        return store.bitpos(bit != 0, range, position);
    });
    if (!ok) position = -1;
    return ok;
}

} // namespace dao
} // namespace pkv
