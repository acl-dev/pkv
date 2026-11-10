#pragma once
#include "compact_zset.h"
#include "validated_page_cache.h"
#include <bitset>
#include <vector>

namespace pkv {
namespace compact_set {
using compact_zset::byte;
using compact_zset::check;
using compact_zset::member_view;
using compact_zset::write_offset;
using compact_zset::wide_offsets_flag;
using compact_zset::deadline_flag;
using compact_zset::narrow_payload;
constexpr unsigned legacy_max_members = 63;
constexpr size_t legacy_payload_limit = 1024;
constexpr unsigned max_members = 128;
constexpr size_t payload_limit = 4096;
using erase_mask = std::bitset<max_members>;

// Both formats prefix each three-byte member with its length.
inline bool has_three_byte_offsets(const char* data, unsigned count) {
    static const unsigned char offsets[] = {
        0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 60,
        64, 68, 72, 76, 80, 84, 88, 92, 96, 100, 104, 108, 112, 116, 120, 124,
        128, 132, 136, 140, 144, 148, 152, 156, 160, 164, 168, 172, 176, 180, 184, 188,
        192, 196, 200, 204, 208, 212, 216, 220, 224, 228, 232, 236, 240, 244, 248, 252,
    };
    return count <= sizeof(offsets) && std::memcmp(data, offsets, count) == 0;
}

inline bool matches(const std::string& bytes) {
    return bytes.size() >= 4 && std::memcmp(bytes.data(), "PMR", 3) == 0 &&
        (byte(bytes[3]) == 6 || byte(bytes[3]) == 8);
}

inline uint64_t read_member_word(const char* data) {
    uint64_t value;
    std::memcpy(&value, data, sizeof(value));
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return __builtin_bswap64(value);
#elif defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return value;
#else
    return (uint64_t(byte(data[0])) << 56) | (uint64_t(byte(data[1])) << 48) |
        (uint64_t(byte(data[2])) << 40) | (uint64_t(byte(data[3])) << 32) |
        (uint64_t(byte(data[4])) << 24) | (uint64_t(byte(data[5])) << 16) |
        (uint64_t(byte(data[6])) << 8) | byte(data[7]);
#endif
}

inline bool short_member_less(member_view left, member_view right) {
    const auto length = std::min(left.size(), right.size());
    if (length < 8 || length > 32) return left < right;
    size_t position = 0;
    do {
        const auto a = read_member_word(left.data() + position);
        const auto b = read_member_word(right.data() + position);
        if (a != b) return a < b;
        position += 8;
    } while (position + 8 <= length);
    if (position < length) {
        // The overlapping tail stays inside both members, including length 9.
        const auto a = read_member_word(left.data() + length - 8);
        const auto b = read_member_word(right.data() + length - 8);
        if (a != b) return a < b;
    }
    return left.size() < right.size();
}

// Version 8 separates the count byte from flags. Keep version 6 for values
// fitting its original bounds; its decoder retains those bounds as well.
inline void write_header(std::string& out, unsigned count, size_t width,
    int64_t deadline, bool extended) {
    out.assign(extended ? "PMR\10" : "PMR\6", 4);
    out.push_back(static_cast<char>((extended ? 0 : count) |
        (width == 2 ? wide_offsets_flag : 0) |
        (deadline >= 0 ? deadline_flag : 0)));
    if (extended) out.push_back(static_cast<char>(count));
    if (deadline >= 0) {
        char expiry[8];
        compact_zset::write_deadline(expiry, deadline);
        out.append(expiry, sizeof(expiry));
    }
}

template<class Member>
inline std::string encode(Member* members, size_t count, int64_t deadline) {
    if (!count || count > max_members) return {};
    size_t payload = 0;
    for (size_t i = 0; i < count; ++i) {
        const auto& member = members[i];
        if (member.size() > 255) return {};
        payload += member.size() + 1;
    }
    if (payload > payload_limit) return {};
    const size_t width = payload > narrow_payload ? 2 : 1;
    std::sort(members, members + count);
    const bool extended = count > legacy_max_members || payload > legacy_payload_limit;
    std::string bytes;
    const auto offsets = (extended ? 6U : 5U) + (deadline < 0 ? 0U : 8U);
    bytes.reserve(offsets + count * width + payload);
    write_header(bytes, static_cast<unsigned>(count), width, deadline, extended);
    const auto start = offsets + count * width;
    bytes.resize(start);
    for (size_t i = 0; i < count; ++i) {
        write_offset(&bytes[offsets + i * width], width, bytes.size() - start);
        bytes.push_back(static_cast<char>(members[i].size()));
        bytes.append(members[i].data(), members[i].size());
    }
    return bytes;
}

inline std::string encode(std::vector<std::string>& members, int64_t deadline = -1) {
    return encode(members.data(), members.size(), deadline);
}

class view {
    struct validation_result { bool fixed_three; };
    using validation_cache = detail::validated_page_cache<validation_result,
        payload_limit + 2 * max_members + 14>;
public:
    explicit view(const std::string& bytes)
        : bytes_(bytes), extended_(bytes.size() > 3 && byte(bytes[3]) == 8),
          count_(extended_ ? (bytes.size() > 5 ? byte(bytes[5]) : 0) :
              (bytes.size() > 4 ? byte(bytes[4]) & 63 : 0)),
          width_(bytes.size() > 4 && (byte(bytes[4]) & wide_offsets_flag) ? 2 : 1),
          offsets_((extended_ ? 6U : 5U) +
              (bytes.size() > 4 && (byte(bytes[4]) & deadline_flag) ? 8U : 0U)),
          start_(offsets_ + count_ * width_) {
        check(matches(bytes_) && count_ &&
            count_ <= (extended_ ? max_members : legacy_max_members) &&
            (!extended_ || !(byte(bytes[4]) & 63)) &&
            start_ < bytes_.size() && payload_size() <=
                (width_ == 1 ? narrow_payload :
                    (extended_ ? payload_limit : legacy_payload_limit)));
        if (extended_) {
            validation_cache cache;
            validation_result saved{};
            if (cache.lookup(bytes_, 0, saved)) {
                fixed_three_ = saved.fixed_three;
            } else {
                validate_members();
                remember_repeated(cache);
            }
        } else validate_members();
    }

    unsigned count() const { return count_; }
    size_t payload_size() const { return bytes_.size() - start_; }
    int64_t deadline() const {
        const auto header = extended_ ? 6U : 5U;
        if (offsets_ == header) return -1;
        uint64_t value = 0;
        for (size_t i = header; i < offsets_; ++i)
            value = (value << 8) | byte(bytes_[i]);
        check(value <= INT64_MAX);
        return static_cast<int64_t>(value);
    }
    void replace_deadline(std::string& bytes, int64_t next) const {
        check(&bytes == &bytes_ && next >= -1);
        const auto header = extended_ ? 6U : 5U;
        if (next < 0) {
            if (offsets_ != header) bytes.erase(header, 8);
        } else {
            if (offsets_ == header) bytes.insert(header, 8, char{});
            compact_zset::write_deadline(&bytes[header], next);
        }
        bytes[4] = static_cast<char>((extended_ ? 0 : count_) | (width_ == 2 ? wide_offsets_flag : 0) |
            (next >= 0 ? deadline_flag : 0));
    }
    member_view at(unsigned i) const {
        if (fixed_three_) return {bytes_.data() + start_ + 4 * i + 1, 3};
        const auto start = start_ + offset_at(i);
        const auto end = i + 1 < count_ ?
            start_ + offset_at(i + 1) : bytes_.size();
        check(start < end && end <= bytes_.size() &&
            byte(bytes_[start]) == end - start - 1);
        return {bytes_.data() + start + 1, end - start - 1};
    }
    unsigned find(member_view member, unsigned* insertion = nullptr) const {
        unsigned first = 0, last = count_;
        while (first < last) {
            const auto mid = first + (last - first) / 2;
            const auto order = at(mid).compare(member);
            if (order == 0) return mid;
            if (order < 0) first = mid + 1;
            else last = mid;
        }
        if (insertion) *insertion = first;
        return count_;
    }
    bool contains(member_view member) const { return find(member) < count_; }

    // An empty result means that the member already exists.
    bool insert(member_view member, std::string& out) const {
        unsigned index = 0;
        if (find(member, &index) < count_) return true;
        if (count_ == max_members || member.size() >= 256) return false;

        const auto added = member.size() + 1;
        const auto size = payload_size() + added;
        if (size > payload_limit) return false;
        const auto split = index < count_ ? offset_at(index) : payload_size();
        const size_t width = size > narrow_payload ? 2 : width_;
        const auto count = count_ + 1;
        const bool extended = extended_ || count > legacy_max_members || size > legacy_payload_limit;
        const auto header_size = offsets_ + (extended && !extended_ ? 1U : 0U);
        out.reserve(header_size + count * width + size);
        write_header(out, count, width, deadline(), extended);
        out.resize(header_size + count * width);
        auto* offsets = &out[header_size];
        if (width == width_) {
            std::memcpy(offsets, bytes_.data() + offsets_, index * width);
        } else {
            for (unsigned i = 0; i < index; ++i)
                write_offset(offsets + i * width, width, offset_at(i));
        }
        write_offset(offsets + index * width, width, split);
        for (unsigned i = index; i < count_; ++i)
            write_offset(offsets + (i + 1) * width, width, offset_at(i) + added);
        out.append(bytes_, start_, split);
        out.push_back(static_cast<char>(member.size()));
        out.append(member.data(), member.size());
        out.append(bytes_, start_ + split, std::string::npos);
        return true;
    }

    std::string without(const erase_mask& removed, unsigned count) const {
        std::string out(bytes_, 0, offsets_);
        out[4] = static_cast<char>((byte(out[4]) & (deadline_flag | wide_offsets_flag)) |
            (extended_ ? 0 : count));
        if (extended_) out[5] = static_cast<char>(count);
        const auto payload = offsets_ + count * width_;
        out.reserve(payload + payload_size());
        out.resize(payload);
        unsigned next = 0;
        for (unsigned i = 0; i < count_; ++i) {
            if (removed[i]) continue;
            const auto item = at(i);
            write_offset(&out[offsets_ + next++ * width_], width_, out.size() - payload);
            out.append(item.data() - 1, item.size() + 1);
        }
        return out;
    }
    std::string without(uint64_t removed, unsigned count) const {
        return without(erase_mask(removed), count);
    }

private:
    void validate_members() {
        fixed_three_ = width_ == 1 && payload_size() == count_ * 4 &&
            has_three_byte_offsets(bytes_.data() + offsets_, count_);
        if (fixed_three_) {
            uint32_t previous = 0;
            const char* record = bytes_.data() + start_;
            for (unsigned i = 0; i < count_; ++i, record += 4) {
                const auto member = three_bytes(record + 1);
                check(record[0] == 3 && (!i || previous < member));
                previous = member;
            }
            return;
        }
        // Choose once per value so long members retain the generic loop.
        if (extended_ && payload_size() <= count_ * 33U)
            validate_variable_members<true>();
        else validate_variable_members<false>();
    }
    template<bool ShortWords>
    void validate_variable_members() {
        check(offset_at(0) == 0);
        size_t cursor = start_;
        member_view previous;
        for (unsigned i = 0; i < count_; ++i) {
            const auto end = i + 1 < count_ ? start_ + offset_at(i + 1) : bytes_.size();
            check(cursor < end && end <= bytes_.size() &&
                byte(bytes_[cursor]) == end - cursor - 1);
            const member_view member(bytes_.data() + cursor + 1, end - cursor - 1);
            check(!i || (ShortWords ? short_member_less(previous, member) :
                previous < member));
            previous = member; cursor = end;
        }
    }

    void remember_repeated(validation_cache& cache) const {
        if (!cache.available()) return;
        struct admission { size_t size; uint64_t first, middle, last; };
        static thread_local admission previous{};
        admission current{bytes_.size(), 0, 0, 0};
        const auto first = at(0);
        std::memcpy(&current.first, first.data(), std::min<size_t>(8, first.size()));
        const auto length = std::min<size_t>(8, bytes_.size());
        std::memcpy(&current.middle, bytes_.data() + (bytes_.size() - length) / 2, length);
        std::memcpy(&current.last, bytes_.data() + bytes_.size() - length, length);
        // Samples only decide whether to retain an already validated value.
        // Every cache reuse still compares ALL bytes, never these hints.
        if (current.size == previous.size && current.first == previous.first &&
            current.middle == previous.middle && current.last == previous.last)
            cache.remember(bytes_, 0, validation_result{fixed_three_});
        previous = current;
    }

    size_t offset_at(unsigned i) const {
        const auto* data = bytes_.data() + offsets_ + i * width_;
        return width_ == 1 ? byte(data[0]) :
            (size_t(byte(data[0])) << 8) | byte(data[1]);
    }

    static uint32_t three_bytes(const char* data) {
        return (uint32_t(byte(data[0])) << 16) |
            (uint32_t(byte(data[1])) << 8) | byte(data[2]);
    }

    const std::string& bytes_;
    bool extended_;
    bool fixed_three_ = false;
    unsigned count_;
    size_t width_;
    size_t offsets_;
    size_t start_;
};
} // namespace compact_set
} // namespace pkv
