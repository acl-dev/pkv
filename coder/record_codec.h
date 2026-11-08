#pragma once
#include <map>
#include <functional>
#include <cmath>
#include <set>
#include <memory>
#include <string>
#include <vector>
#include "common/redis_list_util.h"
#include "common/redis_scan.h"
#include "common/redis_string_util.h"
#include "common/zset_value.h"
#include "common/zset_range.h"
#include "common/redis_double.h"
#include "common/set_sampling.h"

namespace pkv {

enum class value_kind { string, hash, list, set, zset, stream };
struct record_header {
    value_kind kind = value_kind::string;
    long long expires_at_ms = -1;
};
struct value_record {
    record_header header;
    std::string string_value;
    std::vector<std::string> list_value;
    std::set<std::string> set_value;
    std::map<std::string, double> zset_value;
    std::map<std::string, std::string> hash_value;
};

enum class field_status { found, missing, invalid };
enum class codec_status { ok, invalid_record, unsupported_version, out_of_memory, unsupported_precision };

// An owning decoded view. A codec can look up a single hash field without
// materializing every field into a map. No parser-specific types escape here.
class record_view {
public:
    explicit record_view(const record_header& header) : header_(header) {}
    virtual ~record_view() = default;
    const record_header& header() const { return header_; }
    // Metadata-only views refer to native pages outside the encoded root.
    // Opaque root transfers and full payload access must decline these views.
    virtual bool paged_payload() const { return false; }
    // Opt in when this legacy view can enumerate a native-compatible payload.
    // Opaque/read-only extensions retain byte-preserving COPY/RENAME
    // by default.
    // Importers still validate members, limits and complete traversal.
    virtual bool native_importable() const { return false; }
    virtual bool string_value(std::string& out) const = 0;
    // Validate the complete logical payload before transferring opaque bytes.
    // Existing codecs retain a materializing fallback; no encoding is required.
    virtual bool valid_payload() const {
        value_record record;
        switch (header_.kind) {
        case value_kind::string: return string_value(record.string_value);
        case value_kind::hash: return hash_values(record.hash_value);
        case value_kind::list: return list_values(record.list_value) && !record.list_value.empty();
        case value_kind::set: return set_values(record.set_value) && !record.set_value.empty();
        case value_kind::zset:
            if (!zset_values(record.zset_value) || record.zset_value.empty()) return false;
            for (const auto& member : record.zset_value) if (std::isnan(member.second)) return false;
            return true;
        case value_kind::stream: return false;
        }
        return false;
    }
    // Implementations can avoid materializing the entire string for these reads.
    virtual bool string_length(size_t& out) const {
        std::string value;
        if (!string_value(value)) return false;
        out = value.size();
        return true;
    }
    virtual bool string_range(long long start, long long end, std::string& out) const {
        std::string value;
        if (!string_value(value)) return false;
        size_t offset, count;
        if (!string_slice(value.size(), start, end, offset, count)) out.clear();
        else out.assign(value, offset, count);
        return true;
    }
    virtual field_status hash_field(const std::string& name, std::string& out) const = 0;
    virtual bool hash_size(size_t& out) const {
        std::map<std::string, std::string> values;
        if (!hash_values(values)) return false;
        out = values.size();
        return true;
    }
    virtual field_status hash_field_length(const std::string& name, size_t& out) const {
        std::string value;
        const auto status = hash_field(name, value);
        out = value.size();
        return status;
    }
    virtual bool hash_values(std::map<std::string, std::string>& out) const = 0;
    using hash_visitor =
        std::function<bool(const std::string&, const std::string&)>;
    // A visitor may stop early. Legacy codecs may override this fallback to
    // migrate without constructing a second full owning field map.
    virtual bool hash_visit(const hash_visitor& visitor) const {
        std::map<std::string, std::string> fields;
        if (!hash_values(fields)) return false;
        for (const auto& field : fields)
            if (!visitor(field.first, field.second)) break;
        return true;
    }
    // Optional accessors keep existing codecs source-compatible; unsupported
    // list payloads must return false instead of silently decoding as a hash.
    virtual bool list_values(std::vector<std::string>&) const { return false; }
    virtual bool list_size(size_t& out) const {
        std::vector<std::string> values;
        if (!list_values(values)) return false;
        out = values.size();
        return true;
    }
    virtual bool list_range(long long start, long long end, std::vector<std::string>& out) const {
        std::vector<std::string> values;
        if (!list_values(values)) return false;
        size_t offset, count;
        out.clear();
        if (list_slice(values.size(), start, end, offset, count))
            out.assign(values.begin() + offset, values.begin() + offset + count);
        return true;
    }
    virtual bool list_positions(const std::string& value, const list_search& options, std::vector<long long>& out) const {
        std::vector<std::string> values;
        if (!list_values(values)) return false;
        return find_list_positions(values.size(), options,
            [&](size_t index) { return values[index] == value; }, out);
    }
    virtual bool set_values(std::set<std::string>&) const { return false; }
    virtual bool set_size(size_t& out) const {
        std::set<std::string> values;
        if (!set_values(values)) return false;
        out = values.size();
        return true;
    }
    virtual bool set_contains(const std::string& member, bool& out) const {
        std::vector<long long> result;
        if (!set_membership({member}, result) || result.size() != 1) return false;
        out = result.front() != 0;
        return true;
    }
    virtual bool set_membership(const std::vector<std::string>& members, std::vector<long long>& out) const {
        std::set<std::string> values;
        out.clear();
        if (!set_values(values)) return false;
        out.reserve(members.size());
        for (const auto& member : members) out.push_back(values.count(member) ? 1 : 0);
        return true;
    }
    virtual bool hash_scan(const scan_options& options, scan_page& out) const {
        std::map<std::string, std::string> fields;
        if (!hash_values(fields)) return false;
        scan_collection(fields, [](const std::pair<const std::string, std::string>& item) -> const std::string& { return item.first; },
            [&](const std::pair<const std::string, std::string>& item, std::vector<std::string>& values) {
                values.push_back(item.first);
                if (!options.no_values) values.push_back(item.second);
            }, options, out);
        return true;
    }
    // A visitor returning false ends traversal successfully. Views must keep
    // member references valid during the callback only; callbacks must not mutate.
    virtual bool set_visit(const std::function<bool(const std::string&)>& visit) const {
        std::set<std::string> members;
        if (!set_values(members)) return false;
        for (const auto& member : members) if (!visit(member)) break;
        return true;
    }
    virtual bool set_scan(const scan_options& options, scan_page& out) const {
        std::set<std::string> members;
        if (!set_values(members)) return false;
        scan_collection(members, [](const std::string& member) -> const std::string& { return member; },
            [](const std::string& member, std::vector<std::string>& values) { values.push_back(member); }, options, out);
        return true;
    }
    virtual bool zset_values(std::map<std::string, double>&) const { return false; }
    virtual bool zset_lookup(const std::string& member, zset_score& out) const {
        out = zset_score();
        std::vector<zset_score> scores;
        if (!zset_scores({member}, scores) || scores.size() != 1) return false;
        out = scores.front(); return true;
    }
    virtual bool zset_visit(const std::function<bool(const std::string&, double)>& visit) const {
        std::map<std::string, double> members;
        if (!zset_values(members)) return false;
        for (const auto& member : members) if (!visit(member.first, member.second)) break;
        return true;
    }
    virtual bool zset_scan(const scan_options& options, scan_page& out) const {
        out = scan_page();
        std::map<std::string, double> members;
        if (!zset_values(members)) return false;
        scan_collection(members, [](const std::pair<const std::string, double>& item) -> const std::string& { return item.first; },
            [](const std::pair<const std::string, double>& item, std::vector<std::string>& values) {
                values.push_back(item.first);
                values.push_back(format_redis_double(item.second));
            }, options, out);
        return true;
    }
    virtual bool zset_sample(unsigned long long count, bool repeat, std::vector<zset_member>& out) const {
        out.clear();
        std::map<std::string, double> members;
        if (!zset_values(members)) return false;
        return sample_zset(members, count, repeat, set_sampling_engine(), out);
    }
    virtual bool zset_size(size_t& out) const {
        std::map<std::string, double> values;
        if (!zset_values(values)) return false;
        out = values.size(); return true;
    }
    virtual bool zset_scores(const std::vector<std::string>& members, std::vector<zset_score>& out) const {
        out.clear();
        std::map<std::string, double> values;
        if (!zset_values(values)) return false;
        for (const auto& member : members) {
            zset_score score;
            auto it = values.find(member);
            if (it != values.end()) { score.found = true; score.score = it->second; }
            out.push_back(score);
        }
        return true;
    }
    virtual bool zset_range(long long start, long long end, bool reverse, std::vector<zset_member>& out) const {
        out.clear();
        std::map<std::string, double> values;
        if (!zset_values(values)) return false;
        auto ordered = ordered_zset(values);
        if (reverse) std::reverse(ordered.begin(), ordered.end());
        size_t offset, count;
        if (list_slice(ordered.size(), start, end, offset, count))
            out.assign(ordered.begin() + offset, ordered.begin() + offset + count);
        return true;
    }
    virtual bool zset_select(const zset_query& query, std::vector<zset_member>& out) const {
        if (query.mode == zrange_mode::rank) return zset_range(query.start, query.end, query.reverse, out);
        std::map<std::string, double> values;
        if (!zset_values(values)) return false;
        select_zset(values, query, out);
        return true;
    }
    virtual bool zset_count(const zset_query& query, long long& count) const {
        std::vector<zset_member> values;
        if (!zset_select(query, values)) return false;
        count = static_cast<long long>(values.size()); return true;
    }
    virtual bool zset_position(const std::string& member, bool reverse, zset_rank& out) const {
        out = zset_rank();
        std::vector<zset_member> values;
        if (!zset_range(0, LLONG_MAX, reverse, values)) return false;
        for (size_t i = 0; i < values.size(); ++i) if (values[i].member == member) {
            out.found = true; out.rank = static_cast<long long>(i); out.score = values[i].score; break;
        }
        return true;
    }
private:
    record_header header_;
};

class record_codec {
public:
    virtual ~record_codec() = default;
    // Native PLR/PMR roots have storage-wide signatures, independent of the
    // legacy payload codec. Decoding metadata never reads/materializes pages.
    codec_status decode_stored(const std::string& bytes,
        std::unique_ptr<record_view>& out) const;
    // Read storage metadata; retain a payload view only for legacy codecs.
    // native classifies signatures, not validity; always check the status.
    codec_status open_stored(const std::string& bytes, record_header& header,
        std::unique_ptr<record_view>& legacy, bool& native) const;
    // Inspect storage metadata without allocating a native payload view.
    // Legacy codecs retain their existing owning-view decode path.
    codec_status stored_header(const std::string& bytes,
        record_header& out) const;
    virtual codec_status decode(const std::string& bytes, std::unique_ptr<record_view>& out) const = 0;
    virtual codec_status encode(const value_record& record, std::string& out) const = 0;
};

std::shared_ptr<const record_codec> default_record_codec();
long long unix_time_ms();

} // namespace pkv
