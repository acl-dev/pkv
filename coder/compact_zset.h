#pragma once
#include "zset_page.h"
#include "validated_page_cache.h"
#include <algorithm>
#include <array>
#include <bitset>
#include <cmath>
#include <cstring>
#include <numeric>
#include <stdexcept>
#include <vector>
namespace pkv { namespace compact_zset {
class member_view {
public:
    member_view() = default;
    member_view(const char* data, size_t size) : data_(data), size_(size) {}
    member_view(const std::string& value) : member_view(value.data(), value.size()) {}

    int compare(member_view other) const {
        const auto length = std::min(size_, other.size_);
        const int order = length ? std::memcmp(data_, other.data_, length) : 0;
        return order ? order : (size_ > other.size_) - (size_ < other.size_);
    }

    bool operator<(member_view other) const { return compare(other) < 0; }
    const char* data() const { return data_; }
    size_t size() const { return size_; }
    std::string str() const { return std::string(data_, size_); }

private:
    const char* data_ = "";
    size_t size_ = 0;
};

inline bool matches(const std::string& value) {
    return value.size() >= 4 && value.compare(0, 3, "PZR") == 0 &&
        (static_cast<unsigned char>(value[3]) == 3 ||
            static_cast<unsigned char>(value[3]) == 5);
}
using entry = std::pair<std::string, double>;
using entry_view = std::pair<member_view, double>;

inline void check(bool ok) {
    if (!ok) throw std::runtime_error("invalid compact sorted set");
}

inline unsigned byte(char c) { return static_cast<unsigned char>(c); }

inline void write_deadline(char* out, int64_t deadline) {
    for (unsigned i = 0; i < 8; ++i)
        out[i] = static_cast<char>(uint64_t(deadline) >> (56 - 8 * i));
}

inline bool compact_score(double score) {
    return score >= -64 && score <= 63 && score == std::trunc(score) &&
        !(score == 0 && std::signbit(score));
}

// Preserve exact doubles on the 1/256 grid using a biased 16-bit integer.
inline bool fixed_score(double score) {
    return score >= -128 && score < 128 &&
        !(score == 0 && std::signbit(score)) &&
        score * 256 == std::trunc(score * 256);
}

inline bool integer_score(double score) {
    return score >= -32768 && score <= 32767 &&
        score == std::trunc(score) && !(score == 0 && std::signbit(score));
}

enum : unsigned { double_tag = 128, fixed_tag = 129, integer_tag = 130 };
enum : size_t { narrow_payload = 256, wide_payload = 512,
    legacy_payload_limit = 2048, zset_payload_limit = 12288 };
enum : unsigned { legacy_max_members = 63, max_members = 128 };
using erase_mask = std::bitset<max_members>;
struct validation_result {};
using validation_cache = detail::validated_page_cache<validation_result,
    6 + 8 + 3 * max_members + zset_payload_limit, false>;
struct validation_activity { unsigned misses = 0, cooldown = 0; };
inline validation_activity& validation_access() {
    static thread_local validation_activity activity;
    return activity;
}
enum : unsigned { validation_miss_limit = 4, validation_cooldown = 64 };
static_assert(6 + 8 + 3 * max_members + zset_payload_limit <=
    map_page_codec::max_page_bytes, "compact ZSet root must fit one bounded record");
constexpr unsigned wide_offsets_flag = 64;
constexpr unsigned deadline_flag = 128;

inline void write_offset(char* out, size_t width, size_t offset) {
    if (width == 2) *out++ = static_cast<char>(offset >> 8);
    *out = static_cast<char>(offset);
}

inline void write_header(std::string& out, unsigned count, size_t width,
    int64_t deadline, bool extended) {
    out.assign(extended ? "PZR\5" : "PZR\3", 4);
    out.push_back(static_cast<char>((extended ? 0 : count) |
        (width == 2 ? wide_offsets_flag : 0) |
        (deadline >= 0 ? deadline_flag : 0)));
    if (extended) out.push_back(static_cast<char>(count));
    if (deadline >= 0) {
        char expiry[8];
        write_deadline(expiry, deadline);
        out.append(expiry, sizeof(expiry));
    }
}

inline unsigned score_width(unsigned tag) {
    check(tag <= integer_tag);
    return tag < double_tag ? 1 : (tag == double_tag ? 9 : 3);
}

template<class Entry>
inline size_t payload_size(const Entry* values, size_t count) {
    if (!count || count > max_members) return 0;
    size_t bytes = 0;
    for (size_t i = 0; i < count; ++i) {
        const auto& value = values[i];
        if (value.first.size() > 256 || std::isnan(value.second)) return 0;
        const auto score = value.second;
        bytes += value.first.size() + (compact_score(score) ? 1 :
            (fixed_score(score) || integer_score(score) ? 3 : 9));
        if (bytes > zset_payload_limit) return 0;
    }
    return bytes;
}

inline size_t payload_size(const std::vector<entry>& values) {
    return payload_size(values.data(), values.size());
}

inline void append_score(std::string& out, double score) {
    check(!std::isnan(score));
    if (compact_score(score)) {
        out.push_back(static_cast<char>(static_cast<int>(score) + 64));
    } else {
        const bool fixed = fixed_score(score);
        if (fixed || integer_score(score)) {
            const auto biased = static_cast<int>(fixed ? score * 256 : score) + 32768;
            out.push_back(static_cast<char>(fixed ? fixed_tag : integer_tag));
            out.push_back(static_cast<char>(biased >> 8));
            out.push_back(static_cast<char>(biased & 255));
            return;
        }
        out.push_back(static_cast<char>(double_tag));
        check(zset_page_codec::append_value(score, out));
    }
}

// If the value does not fit, leave the input order intact for paged storage.
template<class Entry>
inline std::string try_encode(Entry* values, size_t count, int64_t deadline) {
    const auto payload = payload_size(values, count);
    if (!payload) return {};
    check(deadline >= -1);
    std::sort(values, values + count,
        [](const Entry& a, const Entry& b) { return a.first < b.first; });
    std::array<unsigned, max_members> order;
    const auto end = order.begin() + count;
    std::iota(order.begin(), end, 0u);
    std::sort(order.begin(), end, [&](unsigned a, unsigned b) {
        return values[a].second < values[b].second ||
            (values[a].second == values[b].second && a < b);
    });
    std::string out;
    const bool wide = payload > narrow_payload;
    const size_t width = wide ? 2 : 1;
    const bool extended = count > legacy_max_members || payload > legacy_payload_limit;
    out.reserve((extended ? 6U : 5U) + (deadline >= 0 ? 8 : 0) +
        count * (width + 1) + payload);
    write_header(out, static_cast<unsigned>(count), width, deadline, extended);
    const auto offsets = out.size();
    const auto ranks = offsets + count * width;
    out.resize(ranks + count);
    const auto start = out.size();
    for (size_t i = 0; i < count; ++i) {
        check(!i || member_view(values[i-1].first).compare(values[i].first) != 0);
        write_offset(&out[offsets + i * width], width, out.size() - start);
        out[ranks + i] = static_cast<char>(order[i]);
        append_score(out, values[i].second);
        out.append(values[i].first.data(), values[i].first.size());
    }
    check(out.size() - start <= zset_payload_limit);
    return out;
}

inline std::string try_encode(std::vector<entry>& values, int64_t deadline = -1) {
    return try_encode(values.data(), values.size(), deadline);
}

inline std::string encode(std::vector<entry> values, int64_t deadline = -1) {
    auto bytes = try_encode(values, deadline);
    check(!bytes.empty());
    return bytes;
}

class view {
    const std::string& bytes_;
    bool extended_;
    unsigned count_;
    size_t width_;
    size_t offsets_;
    size_t ranks_;
    size_t start_;
    size_t offset_at(unsigned i) const {
        const auto* encoded = bytes_.data() + offsets_ + i * width_;
        return width_ == 1 ? byte(encoded[0]) :
            (size_t(byte(encoded[0])) << 8) | byte(encoded[1]);
    }
    size_t payload_limit() const {
        return width_ == 1 ? narrow_payload :
            (extended_ ? zset_payload_limit : legacy_payload_limit);
    }
    // The caller checks the score tag and that its bytes fit the record.
    static double decode_score(const char* record, unsigned tag) {
        if (tag < double_tag) return static_cast<int>(tag) - 64;
        if (tag == double_tag) {
            double score;
            check(zset_page_codec::decode_value(record + 1, 8, score));
            return score;
        }
        const auto biased = (byte(record[1]) << 8) | byte(record[2]);
        const auto value = static_cast<int>(biased) - 32768;
        return tag == fixed_tag ? value / 256.0 : value;
    }
public:
    explicit view(const std::string& bytes) : bytes_(bytes),
        extended_(bytes.size() >= 4 && byte(bytes[3]) == 5),
        count_(extended_ ? (bytes.size() >= 6 ? byte(bytes[5]) : 0) :
            (bytes.size() >= 5 ? byte(bytes[4]) & 63 : 0)),
        width_(bytes.size() >= 5 && (byte(bytes[4]) & wide_offsets_flag) ? 2 : 1),
        offsets_((extended_ ? 6U : 5U) +
            (bytes.size() >= 5 && (byte(bytes[4]) & deadline_flag) ? 8U : 0U)),
        ranks_(offsets_ + count_ * width_), start_(ranks_ + count_) {
        check(matches(bytes_) && count_ &&
            count_ <= (extended_ ? max_members : legacy_max_members) &&
            (!extended_ || !(byte(bytes[4]) & 63)) && start_ < bytes_.size() &&
            bytes_.size() - start_ <= payload_limit());
    }

    unsigned count() const { return count_; }
    int64_t deadline() const {
        const auto header = extended_ ? 6U : 5U;
        if (offsets_ == header) return -1;
        uint64_t value = 0;
        for (size_t i = header; i < offsets_; ++i)
            value = (value << 8) | byte(bytes_[i]);
        check(value <= INT64_MAX);
        return static_cast<int64_t>(value);
    }

    // The metadata decoder has already validated the complete root.
    void replace_deadline(std::string& bytes, int64_t next) const {
        check(&bytes == &bytes_ && next >= -1);
        const auto header = extended_ ? 6U : 5U;
        const bool has_deadline = offsets_ != header;
        if (next < 0) {
            if (has_deadline) bytes.erase(header, 8);
        } else {
            if (!has_deadline) bytes.insert(header, 8, '\0');
            write_deadline(&bytes[header], next);
        }
        bytes[4] = static_cast<char>((extended_ ? 0 : count_) | (width_ == 2 ? wide_offsets_flag : 0) |
            (next >= 0 ? deadline_flag : 0));
    }

    std::pair<member_view, size_t> at(unsigned i) const {
        check(i < count_);
        const auto start = start_ + offset_at(i);
        const auto end = i+1 < count_ ? start_ + offset_at(i+1) : bytes_.size();
        check(start < end && end <= bytes_.size());
        const auto tag = byte(bytes_[start]);
        const auto offset = start + score_width(tag);
        check(offset <= end && end - offset <= 256);
        return {{bytes_.data()+offset, end-offset}, start};
    }

    unsigned ordered_index(unsigned rank) const {
        check(rank < count_);
        return byte(bytes_[ranks_ + rank]);
    }

    std::pair<member_view, size_t> ordered_at(unsigned rank) const {
        return at(ordered_index(rank));
    }

    double score(size_t start) const {
        const auto tag = byte(bytes_[start]);
        if (tag < double_tag) return static_cast<int>(tag) - 64;
        check(score_width(tag) <= bytes_.size() - start);
        return decode_score(bytes_.data() + start, tag);
    }

    unsigned member_index(member_view member, size_t* offset = nullptr,
        unsigned* insertion = nullptr) const {
        unsigned first = 0, last = count_;
        while (first < last) {
            const auto mid = first + (last-first)/2;
            const auto item = at(mid);
            const auto cmp = item.first.compare(member);
            if (cmp < 0) first = mid+1;
            else if (cmp > 0) last = mid;
            else {
                if (offset) *offset = item.second;
                return mid;
            }
        }
        if (insertion) *insertion = first;
        return count_;
    }

    // Insert into the validated member and rank tables without decoding
    // unchanged records or re-sorting their relative order.
    bool insert_member(const std::string& member, double score_value,
        unsigned index, std::string& out) const {
        if (count_ == max_members || member.size() > 256) return false;
        std::string encoded_score;
        append_score(encoded_score, score_value);
        const auto added = encoded_score.size() + member.size();
        const auto payload = bytes_.size() - start_;
        if (payload + added > zset_payload_limit) return false;
        if (!extended_ && (count_ + 1 > legacy_max_members ||
            payload + added > legacy_payload_limit)) {
            auto values = entries();
            values.emplace_back(member, score_value);
            auto bytes = try_encode(values, deadline());
            if (bytes.empty()) return false;
            out.swap(bytes);
            return true;
        }
        const auto split = index == count_ ? payload : offset_at(index);
        const size_t width = payload + added > narrow_payload ? 2 : 1;
        const auto count = count_ + 1;
        const auto ranks = offsets_ + count * width;
        std::string bytes(bytes_, 0, offsets_);
        bytes.reserve(ranks + count + payload + added);
        bytes[4] = static_cast<char>((byte(bytes[4]) & deadline_flag) |
            (extended_ ? 0 : count) | (width == 2 ? wide_offsets_flag : 0));
        if (extended_) bytes[5] = static_cast<char>(count);
        bytes.resize(ranks + count);
        for (unsigned i = 0; i < count_; ++i) {
            const bool after = i >= index;
            write_offset(&bytes[offsets_ + (i + after) * width], width,
                offset_at(i) + (after ? added : 0));
        }
        write_offset(&bytes[offsets_ + index * width], width, split);

        unsigned rank = 0;
        for (; rank < count_; ++rank) {
            const auto other = ordered_index(rank);
            const auto previous = score(at(other).second);
            if (previous > score_value || (previous == score_value && other >= index))
                break;
            bytes[ranks + rank] = static_cast<char>(other + (other >= index));
        }
        bytes[ranks + rank] = static_cast<char>(index);
        for (; rank < count_; ++rank) {
            const auto other = ordered_index(rank);
            bytes[ranks + rank + 1] = static_cast<char>(other + (other >= index));
        }

        bytes.append(bytes_, start_, split);
        bytes.append(encoded_score);
        bytes.append(member);
        bytes.append(bytes_, start_ + split, std::string::npos);
        out.swap(bytes);
        return true;
    }

    unsigned rank_of(unsigned index) const {
        const auto* order = bytes_.data() + ranks_;
        const auto rank = std::find(order, order + count_,
            static_cast<char>(index)) - order;
        check(rank < count_);
        return static_cast<unsigned>(rank);
    }

    // The caller has validated the root and counted the removed members.
    std::string without(const erase_mask& erased, unsigned remaining) const {
        check(remaining && remaining <= count_ &&
            erased.count() == count_ - remaining && (erased >> count_).none());
        size_t payload = bytes_.size() - start_;
        for (unsigned i = 0; i < count_; ++i) {
            if (erased[i]) {
                const auto end = i + 1 == count_ ? bytes_.size() - start_ :
                    offset_at(i + 1);
                payload -= end - offset_at(i);
            }
        }
        const size_t width = payload > narrow_payload ? 2 : 1;
        std::string out;
        write_header(out, remaining, width, deadline(),
            remaining > legacy_max_members || payload > legacy_payload_limit);
        const auto offsets = out.size();
        const auto ranks = offsets + remaining * width;
        const auto start = ranks + remaining;
        out.resize(start + payload);
        std::array<unsigned char, max_members> remap;
        unsigned next = 0;
        size_t written = 0;
        for (unsigned i = 0; i < count_; ++i) {
            if (erased[i]) continue;
            const auto offset = offset_at(i);
            const auto end = i + 1 == count_ ? bytes_.size() - start_ :
                offset_at(i + 1);
            write_offset(&out[offsets + next * width], width, written);
            std::memcpy(&out[start + written], bytes_.data() + start_ + offset,
                end - offset);
            remap[i] = static_cast<unsigned char>(next++);
            written += end - offset;
        }
        unsigned rank = 0;
        for (unsigned i = 0; i < count_; ++i) {
            const auto member = ordered_index(i);
            if (!erased[member])
                out[ranks + rank++] = static_cast<char>(remap[member]);
        }
        return out;
    }

    // Edit a validated root owned by the transaction. The rank is already
    // known to the caller, so neither table needs a search or a remap array.
    void erase_one(std::string& bytes, unsigned index, unsigned rank) const {
        check(&bytes == &bytes_ && count_ > 1 && index < count_ &&
            rank < count_ && ordered_index(rank) == index);
        const auto remaining = count_ - 1;
        const auto payload = bytes.size() - start_;
        const auto first = offset_at(index);
        const auto last = index == remaining ? payload : offset_at(index + 1);
        check(first < last && last <= payload);
        const auto removed = last - first;
        const auto kept = payload - removed;
        if ((kept > narrow_payload) != (width_ == 2) ||
            (remaining > legacy_max_members || kept > legacy_payload_limit) != extended_) {
            erase_mask erased;
            erased.set(index);
            bytes = without(erased, remaining);
            return;
        }
        auto* data = &bytes[0];
        for (unsigned i = index; i < remaining; ++i)
            write_offset(data + offsets_ + i * width_, width_, offset_at(i + 1) - removed);
        const auto ranks = ranks_ - width_;
        const auto start = start_ - width_ - 1;
        for (unsigned i = 0; i < remaining; ++i) {
            const auto member = byte(data[ranks_ + i + (i >= rank)]);
            data[ranks + i] = static_cast<char>(member - (member > index));
        }
        std::memmove(data + start, data + start_, first);
        std::memmove(data + start + first, data + start_ + last, payload - last);
        data[4] = static_cast<char>((byte(data[4]) & (deadline_flag | wide_offsets_flag)) |
            (extended_ ? 0 : remaining));
        if (extended_) data[5] = static_cast<char>(remaining);
        bytes.resize(start + kept);
    }

    bool find(member_view member, double& out, unsigned* rank = nullptr) const {
        size_t offset;
        const auto index = member_index(member, &offset);
        if (index == count_) return false;
        out = score(offset);
        if (rank) *rank = rank_of(index);
        return true;
    }

    // The caller validates the view before editing its owned root bytes.
    bool replace_score(std::string& bytes, unsigned index, double next) const {
        check(&bytes == &bytes_);
        const auto start = at(index).second;
        const auto old_size = score_width(byte(bytes_[start]));
        std::string replacement;
        append_score(replacement, next);
        const auto payload = bytes.size() - start_;
        if (payload - old_size + replacement.size() > payload_limit()) return false;
        const auto from = rank_of(index);
        auto to = from;
        const auto before = [&](unsigned other) {
            const auto previous = score(at(other).second);
            return previous < next || (previous == next && other < index);
        };
        const auto search = [&]() {
            // Search the old order with the updated member omitted. The result
            // is its final rank after removing the old entry and inserting it.
            unsigned first = 0, last = count_ - 1;
            while (first < last) {
                const auto middle = first + (last - first) / 2;
                const auto rank = middle + (middle >= from);
                if (before(ordered_index(rank))) first = middle + 1;
                else last = middle;
            }
            return first;
        };
        // Preserve the cheap neighbor walk for small increments. Distant moves
        // use bounded binary search instead of decoding most of the set.
        unsigned steps = 0;
        while (to && !before(ordered_index(to - 1))) {
            --to;
            if (++steps == 4 && to) { to = search(); break; }
        }
        if (to == from) {
            while (to + 1 < count_ && before(ordered_index(to + 1))) {
                ++to;
                if (++steps == 4 && to + 1 < count_) { to = search(); break; }
            }
        }
        if (replacement.size() != old_size) {
            bytes.replace(start, old_size, replacement);
            for (unsigned i = index + 1; i < count_; ++i)
                write_offset(&bytes[offsets_ + i * width_], width_,
                    offset_at(i) - old_size + replacement.size());
        } else {
            std::memcpy(&bytes[start], replacement.data(), replacement.size());
        }
        auto* order = &bytes[ranks_];
        if (to < from) std::memmove(order + to + 1, order + to, from - to);
        if (to > from) std::memmove(order + from, order + from + 1, to - from);
        order[to] = static_cast<char>(index);
        return true;
    }

    std::vector<entry> entries() const {
        validate_fast();
        std::vector<entry> out;
        out.reserve(count_);
        for (unsigned i = 0; i < count_; ++i) {
            const auto value = at(i);
            out.emplace_back(value.first.str(), score(value.second));
        }
        return out;
    }

    // A hit proves only pure validation: every byte must match an owned copy.
    // Writers can reuse a hit without retaining a root they are about to change.
    bool validate_fast(bool retain = true) const {
        if (!extended_) { validate_uncached_fast(); return false; }
        auto& activity = validation_access();
        if (activity.cooldown) {
            --activity.cooldown;
            validate_uncached_fast();
            return false;
        }
        validation_cache cache;
        validation_result saved{};
        if (cache.lookup(bytes_, 0, saved)) {
            activity.misses = 0;
            return true;
        }
        validate_uncached_fast();
        if (cache.available()) {
            if (++activity.misses == validation_miss_limit) {
                activity.misses = 0;
                activity.cooldown = validation_cooldown;
            }
            if (retain && !activity.cooldown) cache.remember(bytes_, 0, saved);
        }
        return false;
    }

    // Requires validation of these unchanged bytes by the caller.
    void remember_validated() const {
        if (!extended_ || validation_access().cooldown) return;
        validation_cache cache;
        cache.remember(bytes_, 0, validation_result{});
    }

    void validate_uncached_fast() const {
        if (width_ != 1 || bytes_.size() - start_ != size_t(count_) * 4) {
            validate();
            return;
        }
        // A fixed-width candidate may still contain variable-size records.
        // Defer to the general validator when its offsets or tags disagree.
        const auto* data = reinterpret_cast<const unsigned char*>(bytes_.data());
        static const unsigned char offsets[64] = {
            0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 60,
            64, 68, 72, 76, 80, 84, 88, 92, 96, 100, 104, 108, 112, 116, 120, 124,
            128, 132, 136, 140, 144, 148, 152, 156, 160, 164, 168, 172, 176, 180, 184, 188,
            192, 196, 200, 204, 208, 212, 216, 220, 224, 228, 232, 236, 240, 244, 248, 252,
        };
        if (count_ > sizeof(offsets) || std::memcmp(data + offsets_, offsets, count_) != 0) {
            validate();
            return;
        }
        uint32_t previous_member = 0;
        for (unsigned i = 0; i < count_; ++i) {
            const auto* record = data + start_ + i * 4;
            if (record[0] >= double_tag) {
                validate();
                return;
            }
            const uint32_t member = (uint32_t(record[1]) << 16) |
                (uint32_t(record[2]) << 8) | record[3];
            check(!i || previous_member < member);
            previous_member = member;
        }
        (void)deadline();
        unsigned previous_rank = 0;
        for (unsigned i = 0; i < count_; ++i) {
            const auto index = data[ranks_+i];
            check(index < count_);
            // Strict ordering of (score, index) also rejects a duplicate.
            const unsigned rank = unsigned(data[start_+index*4]) * max_members + index;
            check(!i || previous_rank < rank);
            previous_rank = rank;
        }
    }

    void validate() const {
        check(offset_at(0) == 0);
        (void)deadline();
        // Only the first count_ entries are read, after all have been written.
        // An exception during decoding exits before the rank-validation loop.
        std::array<double, max_members> scores;
        member_view previous_member;
        const auto* payload = bytes_.data() + start_;
        const auto size = bytes_.size() - start_;
        size_t start = 0;
        for (unsigned i = 0; i < count_; ++i) {
            const auto end = i + 1 == count_ ? size : offset_at(i + 1);
            check(start < end && end <= size);
            const auto* record = payload + start;
            const auto tag = byte(record[0]);
            const auto width = score_width(tag);
            const auto length = end - start;
            check(width <= length && length - width <= 256);
            const member_view member(record + width, length - width);
            check(!i || previous_member < member);
            previous_member = member;
            scores[i] = decode_score(record, tag);
            start = end;
        }
        for (unsigned i = 0, previous = 0; i < count_; ++i) {
            const auto index = byte(bytes_[ranks_+i]);
            check(index < count_);
            // The strict total order of (score, member index) also rejects
            // duplicate indexes, including nonadjacent duplicates and ties.
            if (i) check(scores[previous] < scores[index] ||
                (scores[previous] == scores[index] && previous < index));
            previous = index;
        }
    }
};
}}
