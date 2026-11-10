#pragma once
#include "compact_hash.h"
#include "map_page.h"
#include "validated_page_cache.h"
#include "common/redis_scan.h"
#include <array>
#include <cstdint>

namespace pkv { namespace packed_hash {
// Bounded Hash leaves use the existing compact field codec; only the root
// routes them. map_store selects this format for eligible memory-backed Hashes.
constexpr unsigned min_bits = 4, max_bits = 6;
constexpr unsigned max_groups = 1u << max_bits;
constexpr unsigned max_group_fields = 16, max_fields = 512;
constexpr size_t max_item_bytes = 64;
constexpr size_t max_root_bytes = 7 + 8 + max_groups;
struct root {
    unsigned bits = min_bits;
    uint16_t elements = 0;
    int64_t expires_at_ms = -1;
    std::array<uint8_t, max_groups> sizes{};
    unsigned groups() const {
        return bits >= min_bits && bits <= max_bits ? 1u << bits : 0;
    }
};
bool matches(const std::string& bytes) noexcept;
map_page_status encode_root(const root& value, std::string& out);
map_page_status decode_root(const std::string& bytes, root& out) noexcept;
// Zero selects the existing small/native representation. Density and leaf
// payload limits still need checking before any transaction is published.
unsigned creation_bits(size_t fields) noexcept;
inline unsigned bucket(const char* data, size_t size, unsigned bits) {
    if (bits < min_bits || bits > max_bits) return max_groups;
    return unsigned(scan_bucket(data, size)) & ((1u << bits) - 1);
}
inline unsigned bucket(const std::string& field, unsigned bits) {
    return bucket(field.data(), field.size(), bits);
}

using page_validation_cache = detail::validated_page_cache<unsigned,
    5 + max_group_fields * (2 * max_item_bytes + 3), false>;

// This borrowed view must stay inside the synchronous page inspector. The
// caller supplies validated, owned root metadata for the same locked key.
class page_view : public compact_hash::view {
public:
    page_view(const std::string& bytes, const root& owner, unsigned group,
        const page_validation_cache* cache = nullptr)
        : compact_hash::view(bytes, false), bits_(owner.bits), group_(group) {
        compact_hash::check(group < owner.groups() && owner.sizes[group] &&
            owner.sizes[group] <= max_group_fields &&
            count() == owner.sizes[group] && deadline() == -1);
        unsigned saved;
        if (cache && cache->lookup(bytes, 0, saved) && saved == context()) return;
        validate_fields();
        for (unsigned i = 0; i < count(); ++i) {
            const auto item = compact_hash::view::at(i);
            compact_hash::check(item.first.size() <= max_item_bytes &&
                item.second.size() <= max_item_bytes &&
                bucket(item.first.data(), item.first.size(), owner.bits) == group);
        }
    }
    unsigned context() const { return (bits_ << 8) | group_; }
    std::pair<compact_hash::member_view, compact_hash::member_view>
    at(unsigned i) const {
        compact_hash::check(i < count());
        return compact_hash::view::at(i);
    }
    bool put(compact_hash::member_view field, compact_hash::member_view value,
        std::string& out, bool& added, bool* identical = nullptr) const {
        if (field.size() > max_item_bytes || value.size() > max_item_bytes ||
            bucket(field.data(), field.size(), bits_) != group_)
            return false;
        if (count() == max_group_fields) {
            compact_hash::member_view previous;
            if (!lookup(field, previous)) return false;
        }
        return compact_hash::view::put(field, value, out, added, identical);
    }
    std::string without(uint16_t removed) const {
        const auto valid = (uint32_t{1} << count()) - 1;
        compact_hash::check(!(uint32_t(removed) & ~valid));
        unsigned erased = 0;
        for (unsigned bits = removed; bits; bits >>= 1) erased += bits & 1;
        return erased == count() ? std::string() :
            compact_hash::view::without(removed, count() - erased);
    }
private:
    unsigned bits_, group_;
    using compact_hash::view::replace_deadline;
};
} }
