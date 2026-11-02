#include "json_record_codec.h"
#include "common/execution_gate.h"
#include <yyjson.h>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <cmath>
#include <mutex>
#include "common/redis_double.h"

namespace pkv {
namespace {
struct read_free { void operator()(yyjson_doc* doc) const { yyjson_doc_free(doc); } };
struct write_free { void operator()(yyjson_mut_doc* doc) const { yyjson_mut_doc_free(doc); } };
using read_doc = std::unique_ptr<yyjson_doc, read_free>;
using write_doc = std::unique_ptr<yyjson_mut_doc, write_free>;

bool integer(yyjson_val* value, long long& out) {
    if (!yyjson_is_int(value)) return false;
    if (yyjson_is_uint(value) && yyjson_get_uint(value) > static_cast<unsigned long long>(LLONG_MAX)) return false;
    out = yyjson_get_sint(value);
    return true;
}
long long seconds_ceiling(long long ms) {
    return ms < 0 ? -1 : ms / 1000 + (ms % 1000 != 0);
}

// Sets own one validated membership index. Releasing the JSON document after
// decode avoids retaining both its payload and copied member strings.
class json_set_view : public record_view {
public:
    json_set_view(const record_header& header, std::set<std::string> values)
        : record_view(header), values_(std::move(values)) {}
    bool string_value(std::string&) const override { return false; }
    field_status hash_field(const std::string&, std::string&) const override { return field_status::invalid; }
    bool hash_values(std::map<std::string, std::string>&) const override { return false; }
    bool native_importable() const override { return true; }
    bool valid_payload() const override { return true; }
    bool set_values(std::set<std::string>& out) const override { out = values_; return true; }
    bool set_size(size_t& out) const override { out = values_.size(); return true; }
    bool set_contains(const std::string& member, bool& out) const override {
        out = values_.count(member) != 0;
        return true;
    }
    bool set_membership(const std::vector<std::string>& members, std::vector<long long>& out) const override {
        out.clear();
        out.reserve(members.size());
        for (const auto& member : members) out.push_back(values_.count(member) ? 1 : 0);
        return true;
    }
    bool set_visit(const std::function<bool(const std::string&)>& visit) const override {
        for (const auto& member : values_) if (!visit(member)) break;
        return true;
    }
    bool set_scan(const scan_options& options, scan_page& out) const override {
        scan_collection(values_, [](const std::string& member) -> const std::string& { return member; },
            [](const std::string& member, std::vector<std::string>& values) { values.push_back(member); }, options, out);
        return true;
    }
private:
    std::set<std::string> values_;
};

class json_zset_view : public record_view {
    using item = std::pair<const std::string, double>;
public:
    json_zset_view(const json_zset_view&) = delete;
    json_zset_view& operator=(const json_zset_view&) = delete;
    json_zset_view(const record_header& header, std::map<std::string, double> values)
        : record_view(header), values_(std::move(values)) {}
    bool string_value(std::string&) const override { return false; }
    field_status hash_field(const std::string&, std::string&) const override { return field_status::invalid; }
    bool hash_values(std::map<std::string, std::string>&) const override { return false; }
    bool native_importable() const override { return true; }
    bool valid_payload() const override { return true; }
    bool zset_values(std::map<std::string, double>& out) const override { out = values_; return true; }
    bool zset_lookup(const std::string& member, zset_score& out) const override {
        out = zset_score();
        auto found = values_.find(member);
        if (found != values_.end()) { out.found = true; out.score = found->second; }
        return true;
    }
    bool zset_visit(const std::function<bool(const std::string&, double)>& visit) const override {
        for (const auto& member : values_) if (!visit(member.first, member.second)) break;
        return true;
    }
    bool zset_scan(const scan_options& options, scan_page& out) const override {
        scan_collection(values_, [](const item& value) -> const std::string& { return value.first; },
            [](const item& value, std::vector<std::string>& values) {
                values.push_back(value.first);
                values.push_back(format_redis_double(value.second));
            }, options, out);
        return true;
    }
    bool zset_sample(unsigned long long count, bool repeat, std::vector<zset_member>& out) const override {
        return sample_zset(values_, count, repeat, set_sampling_engine(), out);
    }
    bool zset_size(size_t& out) const override { out = values_.size(); return true; }
    bool zset_scores(const std::vector<std::string>& members, std::vector<zset_score>& out) const override {
        out.clear(); out.reserve(members.size());
        for (const auto& member : members) {
            zset_score score;
            auto it = values_.find(member);
            if (it != values_.end()) { score.found = true; score.score = it->second; }
            out.push_back(score);
        }
        return true;
    }
    bool zset_range(long long start, long long end, bool reverse, std::vector<zset_member>& out) const override {
        out.clear();
        size_t offset, count;
        if (!list_slice(values_.size(), start, end, offset, count)) return true;
        order_values();
        out.reserve(count);
        for (size_t i = offset; i < offset + count; ++i) {
            const auto* value = order_[reverse ? order_.size() - i - 1 : i];
            out.push_back({value->first, value->second});
        }
        return true;
    }
    bool zset_select(const zset_query& query, std::vector<zset_member>& out) const override {
        if (query.mode == zrange_mode::rank) return zset_range(query.start, query.end, query.reverse, out);
        out.clear();
        if (query.offset < 0 || query.limit == 0) return true;
        if (query.mode == zrange_mode::score) {
            const auto bounds = score_bounds(query);
            collect(bounds.first, bounds.second, query, out);
        } else {
            const auto bounds = lex_bounds(query);
            collect(bounds.first, bounds.second, query, out);
        }
        return true;
    }
    bool zset_count(const zset_query& query, long long& out) const override {
        if (query.mode == zrange_mode::rank) return record_view::zset_count(query, out);
        if (query.mode == zrange_mode::score) {
            const auto bounds = score_bounds(query);
            out = static_cast<long long>(bounds.second - bounds.first);
        } else {
            const auto bounds = lex_bounds(query);
            out = static_cast<long long>(std::distance(bounds.first, bounds.second));
        }
        if (query.offset < 0 || query.limit == 0 || query.offset >= out) out = 0;
        else {
            out -= query.offset;
            if (query.limit > 0) out = std::min(out, query.limit);
        }
        return true;
    }
    bool zset_position(const std::string& member, bool reverse, zset_rank& out) const override {
        out = zset_rank();
        auto found = values_.find(member);
        if (found == values_.end()) return true;
        order_values();
        const auto index = static_cast<size_t>(std::lower_bound(order_.begin(), order_.end(), &*found, less) - order_.begin());
        out.found = true; out.rank = static_cast<long long>(reverse ? order_.size() - index - 1 : index); out.score = found->second;
        return true;
    }
private:
    using score_iterator = std::vector<const item*>::const_iterator;
    using lex_iterator = std::map<std::string, double>::const_iterator;
    std::pair<score_iterator, score_iterator> score_bounds(const zset_query& query) const {
        order_values();
        if (zscore_empty(query)) return {order_.end(), order_.end()};
        const auto lower = [&](const zscore_bound& bound) {
            return bound.exclusive ?
                std::upper_bound(order_.begin(), order_.end(), bound.value, [](double score, const item* value) { return score < value->second; }) :
                std::lower_bound(order_.begin(), order_.end(), bound.value, [](const item* value, double score) { return value->second < score; });
        };
        zscore_bound upper = query.max_score;
        upper.exclusive = !upper.exclusive;
        return {lower(query.min_score), lower(upper)};
    }
    std::pair<lex_iterator, lex_iterator> lex_bounds(const zset_query& query) const {
        if (zlex_empty(query)) return {values_.end(), values_.end()};
        auto first = query.min_lex.infinity == -1 ? values_.begin() :
            (query.min_lex.exclusive ? values_.upper_bound(query.min_lex.value) : values_.lower_bound(query.min_lex.value));
        auto last = query.max_lex.infinity == 1 ? values_.end() :
            (query.max_lex.exclusive ? values_.lower_bound(query.max_lex.value) : values_.upper_bound(query.max_lex.value));
        return {first, last};
    }
    static const item& dereference(const item* value) { return *value; }
    static const item& dereference(const item& value) { return value; }
    template<class Iterator> static void collect(Iterator first, Iterator last, const zset_query& query, std::vector<zset_member>& out) {
        long long skip = query.offset;
        while (first != last && skip) { if (query.reverse) --last; else ++first; --skip; }
        while (first != last && (query.limit < 0 || out.size() < static_cast<unsigned long long>(query.limit))) {
            auto current = query.reverse ? --last : first++;
            const auto& value = dereference(*current);
            out.push_back({value.first, value.second});
        }
    }
    static bool less(const item* a, const item* b) { return a->second != b->second ? a->second < b->second : a->first < b->first; }
    std::map<std::string, double> values_;
    void order_values() const {
        std::call_once(order_once_, [this] {
            order_.reserve(values_.size());
            for (const auto& value : values_) order_.push_back(&value);
            std::sort(order_.begin(), order_.end(), less);
        });
    }
    mutable std::once_flag order_once_;
    mutable std::vector<const item*> order_;
};

class json_view : public record_view {
public:
    json_view(const record_header& header, read_doc doc, yyjson_val* data)
        : record_view(header), doc_(std::move(doc)), data_(data) {}
    bool native_importable() const override {
        // Old empty Hash records have no native member tree. Keep their
        // existing opaque transfer behavior rather than creating a missing key.
        return header().kind != value_kind::hash || yyjson_obj_size(data_) != 0;
    }
    bool valid_payload() const override {
        if (header().kind != value_kind::hash) return header().kind == value_kind::string || header().kind == value_kind::list;
        std::set<std::string> fields;
        auto iter = yyjson_obj_iter_with(data_);
        yyjson_val* field;
        while ((field = yyjson_obj_iter_next(&iter))) {
            if (!yyjson_is_str(yyjson_obj_iter_get_val(field)) ||
                !fields.emplace(yyjson_get_str(field), yyjson_get_len(field)).second) return false;
        }
        return true;
    }
    bool string_value(std::string& out) const override {
        if (header().kind != value_kind::string || !yyjson_is_str(data_)) return false;
        out.assign(yyjson_get_str(data_), yyjson_get_len(data_));
        return true;
    }
    bool string_length(size_t& out) const override {
        if (header().kind != value_kind::string || !yyjson_is_str(data_)) return false;
        out = yyjson_get_len(data_);
        return true;
    }
    bool string_range(long long start, long long end, std::string& out) const override {
        size_t length;
        if (!string_length(length)) return false;
        size_t offset, count;
        if (!string_slice(length, start, end, offset, count)) out.clear();
        else out.assign(yyjson_get_str(data_) + offset, count);
        return true;
    }
    field_status hash_field(const std::string& name, std::string& out) const override {
        if (header().kind != value_kind::hash || !yyjson_is_obj(data_)) return field_status::invalid;
        auto field = yyjson_obj_getn(data_, name.data(), name.size());
        if (!field) return field_status::missing;
        if (!yyjson_is_str(field)) return field_status::invalid;
        out.assign(yyjson_get_str(field), yyjson_get_len(field));
        return field_status::found;
    }
    bool hash_size(size_t& out) const override {
        if (header().kind != value_kind::hash || !yyjson_is_obj(data_)) return false;
        out = yyjson_obj_size(data_);
        return true;
    }
    field_status hash_field_length(const std::string& name, size_t& out) const override {
        out = 0;
        if (header().kind != value_kind::hash || !yyjson_is_obj(data_)) return field_status::invalid;
        auto field = yyjson_obj_getn(data_, name.data(), name.size());
        if (!field) return field_status::missing;
        if (!yyjson_is_str(field)) return field_status::invalid;
        out = yyjson_get_len(field);
        return field_status::found;
    }
    bool hash_visit(const hash_visitor& visitor) const override {
        if (header().kind != value_kind::hash || !yyjson_is_obj(data_))
            return false;
        auto iter = yyjson_obj_iter_with(data_);
        yyjson_val* name;
        while ((name = yyjson_obj_iter_next(&iter))) {
            auto value = yyjson_obj_iter_get_val(name);
            if (!yyjson_is_str(value)) return false;
            if (!visitor(std::string(yyjson_get_str(name), yyjson_get_len(name)),
                std::string(yyjson_get_str(value), yyjson_get_len(value))))
                break;
        }
        return true;
    }
    bool hash_values(std::map<std::string, std::string>& out) const override {
        out.clear();
        if (header().kind != value_kind::hash || !yyjson_is_obj(data_)) return false;
        yyjson_obj_iter iter = yyjson_obj_iter_with(data_);
        yyjson_val* name;
        while ((name = yyjson_obj_iter_next(&iter))) {
            auto value = yyjson_obj_iter_get_val(name);
            if (!yyjson_is_str(value) || !out.emplace(
                    std::string(yyjson_get_str(name), yyjson_get_len(name)),
                    std::string(yyjson_get_str(value), yyjson_get_len(value))).second) {
                out.clear();
                return false;
            }
        }
        return true;
    }
    bool hash_scan(const scan_options& options, scan_page& out) const override {
        if (header().kind != value_kind::hash || !yyjson_is_obj(data_)) return false;
        std::array<size_t, 256> sizes{};
        std::set<std::string> names;
        auto iter = yyjson_obj_iter_with(data_);
        yyjson_val* field;
        while ((field = yyjson_obj_iter_next(&iter))) {
            if (!yyjson_is_str(yyjson_obj_iter_get_val(field))) return false;
            auto inserted = names.emplace(yyjson_get_str(field), yyjson_get_len(field));
            if (!inserted.second) return false;
            ++sizes[scan_bucket(*inserted.first)];
        }
        const auto end = scan_window(sizes, options, out);
        iter = yyjson_obj_iter_with(data_);
        while ((field = yyjson_obj_iter_next(&iter))) {
            std::string name(yyjson_get_str(field), yyjson_get_len(field));
            const auto bucket = scan_bucket(name);
            if (bucket < options.cursor || bucket >= end || !scan_accept(name, options)) continue;
            out.values.push_back(std::move(name));
            if (!options.no_values) {
                auto value = yyjson_obj_iter_get_val(field);
                out.values.emplace_back(yyjson_get_str(value), yyjson_get_len(value));
            }
        }
        return true;
    }
    bool list_size(size_t& out) const override {
        if (header().kind != value_kind::list || !yyjson_is_arr(data_)) return false;
        out = yyjson_arr_size(data_);
        return true;
    }
    bool list_range(long long start, long long end, std::vector<std::string>& out) const override {
        out.clear();
        size_t length, offset, count;
        if (!list_size(length)) return false;
        if (!list_slice(length, start, end, offset, count)) return true;
        out.reserve(count);
        // List decoding guarantees a flat string-only array. yyjson indexes
        // flat arrays directly, so ranges need not walk every preceding item.
        for (size_t i = 0; i < count; ++i) {
            auto value = yyjson_arr_get(data_, offset + i);
            if (!yyjson_is_str(value)) { out.clear(); return false; }
            out.emplace_back(yyjson_get_str(value), yyjson_get_len(value));
        }
        return true;
    }
    bool list_values(std::vector<std::string>& out) const override {
        return list_range(0, LLONG_MAX, out);
    }
    bool list_positions(const std::string& value, const list_search& options, std::vector<long long>& out) const override {
        size_t size;
        if (!list_size(size)) return false;
        return find_list_positions(size, options, [&](size_t index) {
            return yyjson_equals_strn(yyjson_arr_get(data_, index), value.data(), value.size());
        }, out);
    }
private:
    read_doc doc_;
    yyjson_val* data_;
};
} // namespace

long long unix_time_ms() {
    return execution_time_ms();
}

std::shared_ptr<const record_codec> default_record_codec() {
    static const auto codec = std::make_shared<const json_record_codec>();
    return codec;
}

codec_status json_record_codec::decode(const std::string& bytes, std::unique_ptr<record_view>& out) const {
    out.reset();
    yyjson_read_err error;
    // No INSITU flag: yyjson owns a copy and does not modify the input bytes.
    read_doc doc(yyjson_read_opts(const_cast<char*>(bytes.data()), bytes.size(),
                                YYJSON_READ_ALLOW_INVALID_UNICODE, nullptr, &error));
    if (!doc) return error.code == YYJSON_READ_ERROR_MEMORY_ALLOCATION
        ? codec_status::out_of_memory : codec_status::invalid_record;
    auto root = yyjson_doc_get_root(doc.get());
    if (!yyjson_is_obj(root)) return codec_status::invalid_record;
    // Duplicate metadata makes version/expiry interpretation ambiguous.
    unsigned seen = 0;
    yyjson_obj_iter iter = yyjson_obj_iter_with(root);
    yyjson_val* key;
    while ((key = yyjson_obj_iter_next(&iter))) {
        unsigned bit = 0;
        if (yyjson_equals_str(key, "T")) bit = 1;
        else if (yyjson_equals_str(key, "E")) bit = 2;
        else if (yyjson_equals_str(key, "D")) bit = 4;
        else if (yyjson_equals_str(key, "V")) bit = 8;
        else if (yyjson_equals_str(key, "P")) bit = 16;
        if (seen & bit) return codec_status::invalid_record;
        seen |= bit;
    }
    long long version = 0;
    if ((seen & 8) && !integer(yyjson_obj_get(root, "V"), version)) return codec_status::invalid_record;
    if (version != 0 && version != 1) return codec_status::unsupported_version;
    // Explicit V:0 is accepted, but precision extensions require V:1.
    if (version == 0 && (seen & 16)) return codec_status::invalid_record;
    if ((seen & 7) != 7) return codec_status::invalid_record;
    record_header header;
    auto type = yyjson_obj_get(root, "T");
    if (!yyjson_is_str(type)) return codec_status::invalid_record;
    if (yyjson_equals_str(type, "S")) header.kind = value_kind::string;
    else if (yyjson_equals_str(type, "H")) header.kind = value_kind::hash;
    else if (version == 1 && yyjson_equals_str(type, "L")) header.kind = value_kind::list;
    else if (version == 1 && yyjson_equals_str(type, "U")) header.kind = value_kind::set;
    else if (version == 1 && yyjson_equals_str(type, "Z")) header.kind = value_kind::zset;
    else return codec_status::unsupported_version;
    long long seconds;
    if (!integer(yyjson_obj_get(root, "E"), seconds) || seconds < -1) return codec_status::invalid_record;
    if (version == 0) {
        if (seconds > LLONG_MAX / 1000) return codec_status::invalid_record;
        header.expires_at_ms = seconds == -1 ? -1 : seconds * 1000;
    } else {
        if (!integer(yyjson_obj_get(root, "P"), header.expires_at_ms) || header.expires_at_ms < -1 ||
            seconds != seconds_ceiling(header.expires_at_ms)) return codec_status::invalid_record;
    }
    auto data = yyjson_obj_get(root, "D");
    if ((header.kind == value_kind::string && !yyjson_is_str(data)) ||
        (header.kind == value_kind::hash && !yyjson_is_obj(data))) return codec_status::invalid_record;
    if (header.kind == value_kind::list) {
        // Empty Redis lists have no key; malformed payloads must not appear as
        // missing keys or be partially returned by a range command.
        if (!yyjson_is_arr(data) || yyjson_arr_size(data) == 0) return codec_status::invalid_record;
        yyjson_arr_iter items = yyjson_arr_iter_with(data);
        yyjson_val* item;
        while ((item = yyjson_arr_iter_next(&items)))
            if (!yyjson_is_str(item)) return codec_status::invalid_record;
    }
    if (header.kind == value_kind::set) {
        if (!yyjson_is_arr(data) || !yyjson_arr_size(data)) return codec_status::invalid_record;
        std::set<std::string> members;
        yyjson_arr_iter member_iter = yyjson_arr_iter_with(data);
        yyjson_val* member;
        while ((member = yyjson_arr_iter_next(&member_iter))) {
            if (!yyjson_is_str(member) || !members.emplace(yyjson_get_str(member), yyjson_get_len(member)).second)
                return codec_status::invalid_record;
        }
        out.reset(new json_set_view(header, std::move(members)));
        return codec_status::ok;
    }
    if (header.kind == value_kind::zset) {
        if (!yyjson_is_arr(data) || !yyjson_arr_size(data)) return codec_status::invalid_record;
        std::map<std::string, double> members;
        auto member_iter = yyjson_arr_iter_with(data);
        yyjson_val* member;
        while ((member = yyjson_arr_iter_next(&member_iter))) {
            if (!yyjson_is_arr(member) || yyjson_arr_size(member) != 2) return codec_status::invalid_record;
            auto name = yyjson_arr_get(member, 0), score = yyjson_arr_get(member, 1);
            double value;
            if (!yyjson_is_str(name) || !yyjson_is_str(score) ||
                !parse_redis_double(std::string(yyjson_get_str(score), yyjson_get_len(score)), value) ||
                !members.emplace(std::string(yyjson_get_str(name), yyjson_get_len(name)), value).second) return codec_status::invalid_record;
        }
        out.reset(new json_zset_view(header, std::move(members)));
        return codec_status::ok;
    }
    out.reset(new json_view(header, std::move(doc), data));
    return codec_status::ok;
}

codec_status json_record_codec::encode(const value_record& record, std::string& out) const {
    if (version_ > 1) return codec_status::unsupported_version;
    if (record.header.expires_at_ms < -1) return codec_status::invalid_record;
    if (version_ == 0 && record.header.expires_at_ms >= 0 && record.header.expires_at_ms % 1000 != 0)
        return codec_status::unsupported_precision;
    write_doc doc(yyjson_mut_doc_new(nullptr));
    if (!doc) return codec_status::out_of_memory;
    auto root = yyjson_mut_obj(doc.get());
    if (!root) return codec_status::out_of_memory;
    yyjson_mut_doc_set_root(doc.get(), root);
    if (version_ == 1 && !yyjson_mut_obj_add_int(doc.get(), root, "V", 1)) return codec_status::out_of_memory;
    const char* type;
    switch (record.header.kind) {
    case value_kind::string: type = "S"; break;
    case value_kind::hash: type = "H"; break;
    case value_kind::list:
        if (version_ == 0) return codec_status::unsupported_version;
        if (record.list_value.empty()) return codec_status::invalid_record;
        type = "L";
        break;
    case value_kind::set:
        if (version_ == 0) return codec_status::unsupported_version;
        if (record.set_value.empty()) return codec_status::invalid_record;
        type = "U";
        break;
    case value_kind::zset:
        if (version_ == 0) return codec_status::unsupported_version;
        if (record.zset_value.empty()) return codec_status::invalid_record;
        type = "Z";
        break;
    default: return codec_status::unsupported_version;
    }
    if (!yyjson_mut_obj_add_str(doc.get(), root, "T", type) ||
        !yyjson_mut_obj_add_int(doc.get(), root, "E", seconds_ceiling(record.header.expires_at_ms)))
        return codec_status::out_of_memory;
    if (version_ == 1 && !yyjson_mut_obj_add_int(doc.get(), root, "P", record.header.expires_at_ms))
        return codec_status::out_of_memory;
    yyjson_mut_val* data;
    if (record.header.kind == value_kind::string) {
        data = yyjson_mut_strn(doc.get(), record.string_value.data(), record.string_value.size());
    } else if (record.header.kind == value_kind::list) {
        data = yyjson_mut_arr(doc.get());
        if (!data) return codec_status::out_of_memory;
        for (const auto& item : record.list_value) {
            auto value = yyjson_mut_strn(doc.get(), item.data(), item.size());
            if (!value || !yyjson_mut_arr_add_val(data, value)) return codec_status::out_of_memory;
        }
    } else if (record.header.kind == value_kind::set) {
        data = yyjson_mut_arr(doc.get());
        if (!data) return codec_status::out_of_memory;
        for (const auto& member : record.set_value) {
            auto value = yyjson_mut_strn(doc.get(), member.data(), member.size());
            if (!value || !yyjson_mut_arr_add_val(data, value)) return codec_status::out_of_memory;
        }
    } else if (record.header.kind == value_kind::zset) {
        data = yyjson_mut_arr(doc.get());
        if (!data) return codec_status::out_of_memory;
        for (const auto& member : record.zset_value) {
            if (std::isnan(member.second)) return codec_status::invalid_record;
            const auto score = format_redis_double(member.second);
            auto pair = yyjson_mut_arr(doc.get());
            auto name = yyjson_mut_strn(doc.get(), member.first.data(), member.first.size());
            // Copy the temporary formatted score into the document's pool.
            auto number = yyjson_mut_strncpy(doc.get(), score.data(), score.size());
            if (!pair || !name || !number || !yyjson_mut_arr_add_val(pair, name) ||
                !yyjson_mut_arr_add_val(pair, number) || !yyjson_mut_arr_add_val(data, pair)) return codec_status::out_of_memory;
        }
    } else {
        data = yyjson_mut_obj(doc.get());
        if (!data) return codec_status::out_of_memory;
        for (const auto& field : record.hash_value) {
            auto name = yyjson_mut_strn(doc.get(), field.first.data(), field.first.size());
            auto value = yyjson_mut_strn(doc.get(), field.second.data(), field.second.size());
            if (!name || !value || !yyjson_mut_obj_add(data, name, value)) return codec_status::out_of_memory;
        }
    }
    if (!data || !yyjson_mut_obj_add_val(doc.get(), root, "D", data)) return codec_status::out_of_memory;
    size_t length;
    std::unique_ptr<char, decltype(&std::free)> bytes(
        yyjson_mut_write(doc.get(), YYJSON_WRITE_ALLOW_INVALID_UNICODE, &length), &std::free);
    if (!bytes) return codec_status::out_of_memory;
    out.assign(bytes.get(), length);
    return codec_status::ok;
}

} // namespace pkv
