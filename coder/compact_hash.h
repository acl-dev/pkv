#pragma once
#include "compact_set.h"
#include <map>

namespace pkv { namespace compact_hash {
using compact_set::byte;
using compact_set::check;
using compact_set::member_view;
using compact_zset::write_offset;
using compact_zset::wide_offsets_flag;
using compact_zset::deadline_flag;
using compact_zset::narrow_payload;
constexpr unsigned max_fields = 63;
constexpr size_t payload_limit = 2048;

inline bool matches(const std::string& bytes) {
    return bytes.compare(0, 4, "PMR\7", 4) == 0;
}

template<class Iterator>
inline std::string encode(Iterator first, Iterator last, size_t count,
    int64_t deadline) {
    if (!count || count > max_fields) return {};
    size_t size = 0;
    for (auto it = first; it != last; ++it) {
        const auto& field = *it;
        if (field.first.size() > 254 || field.second.size() > 254) return {};
        size += 1 + field.first.size() + field.second.size();
    }
    if (size > payload_limit) return {};
    const size_t width = size > narrow_payload ? 2 : 1;
    std::string bytes("PMR\7", 4);
    bytes.push_back(static_cast<char>(count | (width == 2 ? wide_offsets_flag : 0) |
        (deadline >= 0 ? deadline_flag : 0)));
    if (deadline >= 0) {
        char expiry[8];
        compact_zset::write_deadline(expiry, deadline);
        bytes.append(expiry, sizeof(expiry));
    }
    const auto offsets = bytes.size();
    const auto start = offsets + count * width;
    bytes.reserve(start + size);
    bytes.resize(start);
    size_t i = 0;
    for (auto it = first; it != last; ++it) {
        const auto& field = *it;
        write_offset(&bytes[offsets + i++ * width], width, bytes.size() - start);
        bytes.push_back(static_cast<char>(field.first.size()));
        bytes.append(field.first.data(), field.first.size());
        bytes.append(field.second.data(), field.second.size());
    }
    return bytes;
}

inline std::string encode(const std::map<std::string, std::string>& fields,
    int64_t deadline = -1) {
    return encode(fields.begin(), fields.end(), fields.size(), deadline);
}

class view {
public:
    explicit view(const std::string& bytes) : view(bytes, true) {}

protected:
    view(const std::string& bytes, bool validate)
        : bytes_(bytes), count_(bytes.size() > 4 ? byte(bytes[4]) & 63 : 0),
          width_(bytes.size() > 4 && (byte(bytes[4]) & wide_offsets_flag) ? 2 : 1),
          offsets_(bytes.size() > 4 && (byte(bytes[4]) & deadline_flag) ? 13 : 5),
          start_(offsets_ + count_ * width_) {
        check(matches(bytes) && count_ && count_ <= max_fields &&
            start_ < bytes.size() && bytes.size() - start_ <=
                (width_ == 1 ? narrow_payload : payload_limit));
        if (validate) validate_fields();
    }

    void validate_fields() {
        if (validate_fixed_pairs()) {
            fixed_pairs_ = true;
            return;
        }
        check(offset_at(0) == 0);
        member_view previous;
        for (unsigned i = 0; i < count_; ++i) {
            const auto field = at(i).first;
            check(!i || previous < field);
            previous = field;
        }
    }

public:
    unsigned count() const { return count_; }
    int64_t deadline() const {
        if (offsets_ == 5) return -1;
        uint64_t value = 0;
        for (size_t i = 5; i < offsets_; ++i)
            value = (value << 8) | byte(bytes_[i]);
        check(value <= INT64_MAX);
        return static_cast<int64_t>(value);
    }
    void replace_deadline(std::string& bytes, int64_t next) const {
        check(&bytes == &bytes_ && next >= -1);
        if (next < 0) {
            if (offsets_ != 5) bytes.erase(5, 8);
        } else {
            if (offsets_ == 5) bytes.insert(5, 8, char{});
            compact_zset::write_deadline(&bytes[5], next);
        }
        bytes[4] = static_cast<char>(count_ | (width_ == 2 ? wide_offsets_flag : 0) |
            (next >= 0 ? deadline_flag : 0));
    }
    std::pair<member_view, member_view> at(unsigned i) const {
        if (fixed_pairs_) {
            const auto* field = bytes_.data() + start_ + 7 * i + 1;
            return {{field, 3}, {field + 3, 3}};
        }
        const auto start = start_ + offset_at(i);
        const auto end = i + 1 < count_ ? start_ + offset_at(i + 1) :
            bytes_.size();
        check(start < end && end <= bytes_.size());
        const auto length = byte(bytes_[start]);
        check(length < end - start && end - start - 1 - length <= 255);
        const auto field = bytes_.data() + start + 1;
        return {{field, length}, {field + length, end - start - 1 - length}};
    }
    bool lookup(member_view field, member_view& out, unsigned* index = nullptr) const {
        unsigned first = 0, last = count_;
        while (first < last) {
            const auto mid = first + (last - first) / 2;
            const auto item = at(mid);
            const auto order = item.first.compare(field);
            if (order == 0) {
                out = item.second;
                if (index) *index = mid;
                return true;
            }
            if (order < 0) first = mid + 1;
            else last = mid;
        }
        if (index) *index = first;
        return false;
    }

    bool get(member_view field, std::string& out) const {
        member_view value;
        if (!lookup(field, value)) return false;
        out.assign(value.data(), value.size());
        return true;
    }

    // An identical result can leave out untouched when requested by the caller.
    bool put(member_view field, member_view value, std::string& out,
        bool& added, bool* identical = nullptr) const {
        if (field.size() > 254 || value.size() > 254) return false;
        member_view old;
        unsigned index;
        const bool found = lookup(field, old, &index);
        if (!found && count_ == max_fields) return false;

        const bool owned = &out == &bytes_;
        if (identical) *identical = false;
        if (found && old.size() == value.size()) {
            added = false;
            if (identical && old.compare(value) == 0) {
                *identical = true;
                return true;
            }
            const auto offset = old.data() - bytes_.data();
            // A borrowed page needs an owned copy before editing its value.
            if (!owned) out = bytes_;
            if (value.size()) std::memmove(&out[0] + offset, value.data(), value.size());
            return true;
        }

        const auto payload_size = bytes_.size() - start_;
        const auto split = index < count_ ? offset_at(index) : payload_size;
        const auto old_size = found ? 1 + field.size() + old.size() : 0;
        const auto new_size = 1 + field.size() + value.size();
        const auto updated_size = payload_size - old_size + new_size;
        if (updated_size > payload_limit) return false;

        const auto count = count_ + !found;
        const size_t width = updated_size > narrow_payload ? 2 : width_;
        // For an aliased output, finish a replacement before publishing it.
        // Allocation failure must leave the owned input intact.
        std::string replacement;
        auto& target = owned ? replacement : out;
        target.reserve(offsets_ + count * width + updated_size);
        target.assign(bytes_, 0, offsets_);
        target[4] = static_cast<char>((byte(target[4]) & deadline_flag) | count |
            (width == 2 ? wide_offsets_flag : 0));
        target.resize(offsets_ + count * width);
        auto* offsets = &target[offsets_];
        if (width == width_) {
            std::memcpy(offsets, bytes_.data() + offsets_, index * width);
        } else {
            for (unsigned i = 0; i < index; ++i)
                write_offset(offsets + i * width, width, offset_at(i));
        }
        write_offset(offsets + index * width, width, split);
        for (unsigned i = index + found; i < count_; ++i)
            write_offset(offsets + (i + !found) * width, width,
                offset_at(i) - old_size + new_size);
        target.append(bytes_, start_, split);
        target.push_back(static_cast<char>(field.size()));
        target.append(field.data(), field.size());
        target.append(value.data(), value.size());
        target.append(bytes_, start_ + split + old_size, std::string::npos);
        if (owned) out.swap(replacement);
        added = !found;
        return true;
    }

    std::string without(uint64_t removed, unsigned remaining) const {
        std::string out(bytes_, 0, offsets_);
        out[4] = static_cast<char>((byte(out[4]) & (deadline_flag | wide_offsets_flag)) | remaining);
        const auto payload = offsets_ + remaining * width_;
        out.reserve(payload + bytes_.size() - start_);
        out.resize(payload);
        unsigned next = 0;
        for (unsigned i = 0; i < count_; ++i) {
            if (removed & (uint64_t{1} << i)) continue;
            const auto start = start_ + offset_at(i);
            const auto end = i + 1 < count_ ?
                start_ + offset_at(i + 1) : bytes_.size();
            write_offset(&out[offsets_ + next++ * width_], width_, out.size() - payload);
            out.append(bytes_, start, end - start);
        }
        return out;
    }

private:
    size_t offset_at(unsigned i) const {
        const auto* data = bytes_.data() + offsets_ + i * width_;
        return width_ == 1 ? byte(data[0]) :
            (size_t(byte(data[0])) << 8) | byte(data[1]);
    }
    bool validate_fixed_pairs() const {
        static const unsigned char offsets[] = {
            0, 7, 14, 21, 28, 35, 42, 49, 56, 63, 70, 77, 84, 91, 98, 105,
            112, 119, 126, 133, 140, 147, 154, 161, 168, 175, 182, 189,
            196, 203, 210, 217, 224, 231, 238, 245,
        };
        if (width_ != 1 || bytes_.size() - start_ != 7 * count_ ||
            count_ > sizeof(offsets) ||
            std::memcmp(bytes_.data() + offsets_, offsets, count_) != 0)
            return false;
        uint32_t previous = 0;
        const char* record = bytes_.data() + start_;
        for (unsigned i = 0; i < count_; ++i, record += 7) {
            // Equal pair widths can still have different field/value splits.
            if (record[0] != 3) return false;
            const uint32_t field = (uint32_t(byte(record[1])) << 16) |
                (uint32_t(byte(record[2])) << 8) | byte(record[3]);
            check(!i || previous < field);
            previous = field;
        }
        return true;
    }

    const std::string& bytes_;
    bool fixed_pairs_ = false;
    unsigned count_;
    size_t width_;
    size_t offsets_;
    size_t start_;
};
} } // namespace pkv::compact_hash
