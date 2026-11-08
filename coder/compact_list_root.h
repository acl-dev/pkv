#pragma once
#include "compact_set.h"
#include "list_page.h"

namespace pkv { namespace compact_list_root {
using compact_set::byte;
using compact_set::check;
using compact_set::member_view;
using compact_set::write_offset;
using compact_set::wide_offsets_flag;
using compact_set::deadline_flag;
using compact_set::narrow_payload;

constexpr size_t wide_payload = 2048;
constexpr unsigned max_members = 63;

inline bool fits_payload(size_t size, size_t count) {
    // The larger allocation pays off when it holds enough short members.
    return size <= 1024 || (size <= wide_payload && size <= count * 40);
}

inline bool matches(const std::string& bytes) {
    return bytes.compare(0, 4, "PLR\3", 4) == 0;
}

template<class Values>
inline std::string encode_ordered(const Values& values, bool reverse, int64_t deadline) {
    if (values.empty() || values.size() > max_members) return {};
    size_t size = 0;
    for (const auto& value : values) {
        if (value.size() > 255) return {};
        size += value.size() + 1;
    }
    if (!fits_payload(size, values.size())) return {};
    const size_t width = size > narrow_payload ? 2 : 1;
    std::string bytes("PLR\3", 4);
    bytes.push_back(static_cast<char>(values.size() | (width == 2 ? wide_offsets_flag : 0) |
        (deadline >= 0 ? deadline_flag : 0)));
    if (deadline >= 0) {
        char expiry[8];
        compact_zset::write_deadline(expiry, deadline);
        bytes.append(expiry, sizeof(expiry));
    }
    const auto offsets = bytes.size();
    const auto start = offsets + values.size() * width;
    bytes.reserve(start + size);
    bytes.resize(start);
    for (size_t i = 0; i < values.size(); ++i) {
        const auto& value = values[reverse ? values.size() - 1 - i : i];
        write_offset(&bytes[offsets + i * width], width, bytes.size() - start);
        bytes.push_back(static_cast<char>(value.size()));
        bytes.append(value.data(), value.size());
    }
    return bytes;
}

inline std::string encode(const std::vector<member_view>& values, int64_t deadline = -1) {
    return encode_ordered(values, false, deadline);
}

inline std::string encode_push(list_values_view values, bool left) {
    return encode_ordered(values, left, -1);
}

class view {
public:
    explicit view(const std::string& bytes)
        : bytes_(bytes), count_(bytes.size() > 4 ? byte(bytes[4]) & 63 : 0),
          width_(bytes.size() > 4 && (byte(bytes[4]) & wide_offsets_flag) ? 2 : 1),
          offsets_(bytes.size() > 4 && (byte(bytes[4]) & deadline_flag) ? 13 : 5),
          start_(offsets_ + count_ * width_) {
        check(matches(bytes) && count_ && count_ <= max_members &&
            start_ < bytes.size() && bytes.size() - start_ <=
                (width_ == 1 ? narrow_payload : wide_payload));
        fixed_three_ = width_ == 1 && bytes.size() - start_ == count_ * 4 &&
            compact_set::has_three_byte_offsets(bytes.data() + offsets_, count_);
        if (fixed_three_) {
            for (unsigned i = 0; i < count_; ++i)
                check(bytes[start_ + i * 4] == 3);
            return;
        }
        size_t next = start_;
        for (unsigned i = 0; i < count_; ++i) {
            check(next < bytes.size() && offset_at(i) == next - start_);
            next += 1 + byte(bytes[next]);
        }
        check(next == bytes.size());
    }
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
    member_view at(unsigned i) const {
        if (fixed_three_) return {bytes_.data() + start_ + i * 4 + 1, 3};
        const auto begin = start_ + offset_at(i);
        const auto end = i + 1 < count_ ? start_ + offset_at(i + 1) :
            bytes_.size();
        check(begin < end && end <= bytes_.size() &&
            byte(bytes_[begin]) == end - begin - 1);
        return {bytes_.data() + begin + 1, end - begin - 1};
    }
    bool extend(list_values_view values, bool left, std::string& out) const {
        if (values.size() > max_members - count_) return false;
        size_t added = 0;
        for (const auto& value : values) {
            if (value.size() > 255) return false;
            added += value.size() + 1;
        }
        const auto payload = bytes_.size() - start_;
        const auto count = count_ + values.size();
        if (!fits_payload(added + payload, count)) return false;

        check(&out == &bytes_);
        const size_t width = added + payload > narrow_payload ? 2 : width_;
        const auto next_start = offsets_ + count * width;
        out.resize(next_start + added + payload);
        auto* data = &out[0];
        std::memmove(data + next_start + (left ? added : 0), data + start_, payload);
        if (left || width != width_) {
            const size_t shift = left ? values.size() : 0;
            for (unsigned i = count_; i; --i)
                write_offset(data + offsets_ + (shift + i - 1) * width,
                    width, offset_at(i - 1) + (left ? added : 0));
        }
        size_t offset = left ? 0 : payload;
        const auto first = offsets_ + (left ? 0 : count_ * width);
        for (size_t i = 0; i < values.size(); ++i) {
            const auto& value = values[left ? values.size() - 1 - i : i];
            write_offset(data + first + i * width, width, offset);
            auto* item = data + next_start + offset;
            *item++ = static_cast<char>(value.size());
            std::memcpy(item, value.data(), value.size());
            offset += value.size() + 1;
        }
        data[4] = static_cast<char>((byte(data[4]) & deadline_flag) | count |
            (width == 2 ? wide_offsets_flag : 0));
        return true;
    }
    bool replace(unsigned index, member_view value, std::string& out) const {
        const auto old_size = at(index).size();
        const auto begin = start_ + offset_at(index);
        const auto end = begin + 1 + old_size;
        const auto size = bytes_.size() - start_ - old_size + value.size();
        if (value.size() > 255 ||
            (value.size() > old_size && !fits_payload(size, count_))) return false;
        if (old_size == value.size()) {
            out = bytes_;
            out.replace(begin + 1, old_size, value.data(), value.size());
            return true;
        }
        const size_t width = size > narrow_payload ? 2 : width_;
        out.reserve(offsets_ + count_ * width + size);
        out.assign(bytes_, 0, offsets_);
        if (width == 2) out[4] = static_cast<char>(byte(out[4]) | wide_offsets_flag);
        out.resize(offsets_ + count_ * width);
        for (unsigned i = 0; i < count_; ++i) {
            const auto offset = offset_at(i);
            write_offset(&out[offsets_ + i * width], width,
                i > index ? offset - old_size + value.size() : offset);
        }
        out.append(bytes_, start_, begin - start_);
        out.push_back(static_cast<char>(value.size()));
        out.append(value.data(), value.size());
        out.append(bytes_, end, std::string::npos);
        return true;
    }
    std::string slice(unsigned first, unsigned count) const {
        const auto begin = offset_at(first);
        const auto end = first + count < count_ ?
            offset_at(first + count) : bytes_.size() - start_;
        std::string out;
        const size_t width = end - begin > narrow_payload ? 2 : 1;
        out.reserve(offsets_ + count * width + end - begin);
        out.assign(bytes_, 0, offsets_);
        out[4] = static_cast<char>((byte(out[4]) & deadline_flag) | count |
            (width == 2 ? wide_offsets_flag : 0));
        out.resize(offsets_ + count * width);
        for (unsigned i = 0; i < count; ++i)
            write_offset(&out[offsets_ + i * width], width, offset_at(first + i) - begin);
        out.append(bytes_, start_ + begin, end - begin);
        return out;
    }
    std::string without(uint64_t removed, unsigned remaining) const {
        std::string out;
        out.reserve(offsets_ + remaining * width_ + bytes_.size() - start_);
        out.assign(bytes_, 0, offsets_);
        out[4] = static_cast<char>((byte(out[4]) & (deadline_flag | wide_offsets_flag)) | remaining);
        const auto payload = offsets_ + remaining * width_;
        out.resize(payload);
        unsigned next = 0;
        for (unsigned i = 0; i < count_; ++i) {
            if (removed & (uint64_t{1} << i)) continue;
            const auto item = at(i);
            write_offset(&out[offsets_ + next++ * width_], width_, out.size() - payload);
            out.push_back(static_cast<char>(item.size()));
            out.append(item.data(), item.size());
        }
        return out;
    }
private:
    size_t offset_at(unsigned i) const {
        const auto* data = bytes_.data() + offsets_ + i * width_;
        return width_ == 1 ? byte(data[0]) :
            (size_t(byte(data[0])) << 8) | byte(data[1]);
    }

    const std::string& bytes_;
    bool fixed_three_ = false;
    unsigned count_;
    size_t width_;
    size_t offsets_;
    size_t start_;
};
} } // namespace pkv::compact_list_root
