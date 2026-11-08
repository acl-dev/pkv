#include "coder/json_record_codec.h"
#include "common/redis_integer.h"
#include "dao/json/json_string.h"
#include "dao/paged_string.h"
#include "dao/json/json_key.h"
#include "dao/json/json_hash.h"
#include "dao/json/json_list.h"
#include "dao/json/json_set.h"
#include "dao/json/json_scan.h"
#include "dao/json/json_zset.h"
#include "common/redis_double.h"
#include <cmath>
#include <clocale>
#include <thread>
#include <cstring>
#include <limits>
#include "common/set_sampling.h"
#include "common/redis_scan.h"
#include <cstdlib>
#include <vector>
#include <iostream>

static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}

// A different format, implemented without yyjson or any JSON parsing. It is
// deliberately string-only: unsupported value kinds must fail without writing.
class text_view : public pkv::record_view {
public:
    text_view(const pkv::record_header& header, std::string data)
        : record_view(header), data_(std::move(data)) {}
    bool string_value(std::string& out) const override { out = data_; return true; }
    pkv::field_status hash_field(const std::string&, std::string&) const override { return pkv::field_status::invalid; }
    bool hash_values(std::map<std::string, std::string>&) const override { return false; }
private:
    std::string data_;
};
class text_codec : public pkv::record_codec {
public:
    pkv::codec_status encode(const pkv::value_record& value, std::string& out) const override {
        if (value.header.kind != pkv::value_kind::string) return pkv::codec_status::unsupported_version;
        out = "TEXT:" + std::to_string(value.header.expires_at_ms) + "\n" + value.string_value;
        return pkv::codec_status::ok;
    }
    pkv::codec_status decode(const std::string& bytes, std::unique_ptr<pkv::record_view>& out) const override {
        out.reset();
        if (bytes.compare(0, 5, "TEXT:") != 0) return pkv::codec_status::invalid_record;
        const auto end = bytes.find('\n', 5);
        pkv::record_header header;
        if (end == std::string::npos || !pkv::parse_redis_integer(bytes.substr(5, end - 5), header.expires_at_ms))
            return pkv::codec_status::invalid_record;
        out.reset(new text_view(header, bytes.substr(end + 1)));
        return pkv::codec_status::ok;
    }
};

// A read-only view that refuses full collection materialization. Selective
// HMGET and LPOS must stay usable through this extension boundary.
class selective_view : public pkv::record_view {
public:
    explicit selective_view(std::unique_ptr<pkv::record_view> inner, bool fail_zscan = false)
        : record_view(inner->header()), inner_(std::move(inner)), fail_zscan_(fail_zscan) {}
    bool valid_payload() const override { return inner_->valid_payload(); }
    bool string_value(std::string&) const override { return false; }
    pkv::field_status hash_field(const std::string& name, std::string& out) const override {
        return inner_->hash_field(name, out);
    }
    bool hash_values(std::map<std::string, std::string>&) const override {
        require(false, "selective hash read materialized the entire map");
        return false;
    }
    bool list_values(std::vector<std::string>&) const override {
        require(false, "selective list read materialized the entire list");
        return false;
    }
    bool list_positions(const std::string& value, const pkv::list_search& options, std::vector<long long>& out) const override {
        return inner_->list_positions(value, options, out);
    }
    bool hash_scan(const pkv::scan_options& options, pkv::scan_page& out) const override { return inner_->hash_scan(options, out); }
    bool set_scan(const pkv::scan_options& options, pkv::scan_page& out) const override { return inner_->set_scan(options, out); }
    bool zset_values(std::map<std::string, double>&) const override {
        require(false, "selective ZSCAN materialized all members");
        return false;
    }
    bool zset_size(size_t& out) const override { return inner_->zset_size(out); }
    bool zset_sample(unsigned long long count, bool repeat, std::vector<pkv::zset_member>& out) const override {
        if (fail_zscan_) { out = {{"partial", 1}}; return false; }
        return inner_->zset_sample(count, repeat, out);
    }
    bool zset_scan(const pkv::scan_options& options, pkv::scan_page& out) const override {
        if (fail_zscan_) { out.cursor = 17; out.values = {"partial"}; return false; }
        return inner_->zset_scan(options, out);
    }
    bool set_values(std::set<std::string>&) const override {
        require(false, "selective set query materialized all members");
        return false;
    }
    bool set_contains(const std::string& member, bool& out) const override { return inner_->set_contains(member, out); }
    bool set_visit(const std::function<bool(const std::string&)>& callback) const override { return inner_->set_visit(callback); }
    bool zset_lookup(const std::string& member, pkv::zset_score& out) const override {
        if (fail_zscan_) { out.found = true; out.score = 42; return false; }
        return inner_->zset_lookup(member, out);
    }
    bool zset_visit(const std::function<bool(const std::string&, double)>& callback) const override {
        if (fail_zscan_) { callback("partial", 1); return false; }
        return inner_->zset_visit(callback);
    }
    bool set_size(size_t& out) const override { return inner_->set_size(out); }
    bool set_membership(const std::vector<std::string>& values, std::vector<long long>& out) const override {
        return inner_->set_membership(values, out);
    }
private:
    std::unique_ptr<pkv::record_view> inner_;
    bool fail_zscan_;
};
class selective_codec : public pkv::record_codec {
public:
    bool fail_zscan = false;
    pkv::codec_status encode(const pkv::value_record&, std::string&) const override {
        return pkv::codec_status::unsupported_version;
    }
    pkv::codec_status decode(const std::string& bytes, std::unique_ptr<pkv::record_view>& out) const override {
        std::unique_ptr<pkv::record_view> inner;
        const auto status = pkv::json_record_codec().decode(bytes, inner);
        out.reset();
        if (status == pkv::codec_status::ok) out.reset(new selective_view(std::move(inner), fail_zscan));
        return status;
    }
};

class vector_list_view : public pkv::record_view {
public:
    vector_list_view(const pkv::record_header& header, std::vector<std::string> values)
        : record_view(header), values_(std::move(values)) {}
    bool string_value(std::string&) const override { return false; }
    pkv::field_status hash_field(const std::string&, std::string&) const override { return pkv::field_status::invalid; }
    bool hash_values(std::map<std::string, std::string>&) const override { return false; }
    bool list_values(std::vector<std::string>& out) const override { out = values_; return true; }
private:
    std::vector<std::string> values_;
};

class plain_set_view : public pkv::record_view {
public:
    plain_set_view(const pkv::record_header& header, std::set<std::string> values)
        : record_view(header), values_(std::move(values)) {}
    bool string_value(std::string&) const override { return false; }
    pkv::field_status hash_field(const std::string&, std::string&) const override { return pkv::field_status::invalid; }
    bool hash_values(std::map<std::string, std::string>&) const override { return false; }
    bool set_values(std::set<std::string>& out) const override { out = values_; return true; }
private:
    std::set<std::string> values_;
};

class plain_zset_view : public pkv::record_view {
public:
    plain_zset_view(const pkv::record_header& header, std::map<std::string, double> values)
        : record_view(header), values_(std::move(values)) {}
    bool string_value(std::string&) const override { return false; }
    pkv::field_status hash_field(const std::string&, std::string&) const override { return pkv::field_status::invalid; }
    bool hash_values(std::map<std::string, std::string>&) const override { return false; }
    bool zset_values(std::map<std::string, double>& out) const override { out = values_; return true; }
private:
    std::map<std::string, double> values_;
};

int main() {
    using namespace pkv;
    json_record_codec codec;
    json_record_codec legacy(0);
    std::string binary;
    for (int i = 0; i < 256; ++i) binary.push_back(static_cast<char>(i));
    for (auto kind : {value_kind::string, value_kind::hash}) {
        for (long long expiry : {-1LL, 0LL, 1000LL, 1001LL, LLONG_MAX}) {
            value_record record;
            record.header.kind = kind;
            record.header.expires_at_ms = expiry;
            record.string_value = binary;
            record.hash_value = {{"", ""}, {binary, binary}};
            std::string bytes;
            require(codec.encode(record, bytes) == codec_status::ok, "v1 encode failed");
            std::unique_ptr<record_view> view;
            require(codec.decode(bytes, view) == codec_status::ok && view &&
                    view->header().kind == kind && view->header().expires_at_ms == expiry, "v1 header roundtrip failed");
            std::string value;
            if (kind == value_kind::string) {
                require(view->string_value(value) && value == binary, "v1 string bytes lost");
                size_t length;
                require(view->string_length(length) && length == 256, "codec length mismatch");
                require(view->string_range(-2, -1, value) && value == binary.substr(254), "codec range mismatch");
            }
            else {
                require(view->hash_field(binary, value) == field_status::found && value == binary, "v1 hash bytes lost");
                require(view->hash_field("missing", value) == field_status::missing, "missing field lost");
                size_t length;
                require(view->hash_size(length) && length == 2, "codec hash size mismatch");
                require(view->hash_field_length(binary, length) == field_status::found && length == 256,
                        "codec hash field length mismatch");
                require(view->hash_field_length("", length) == field_status::found && length == 0,
                        "empty hash field length mismatch");
                require(view->hash_field_length("absent", length) == field_status::missing,
                        "missing hash field length mismatch");
                std::map<std::string, std::string> fields;
                require(view->hash_values(fields) && fields == record.hash_value, "v1 hash materialization failed");
            }
            if (expiry < 0 || expiry % 1000 == 0) {
                require(legacy.encode(record, bytes) == codec_status::ok && bytes.find("\"V\"") == std::string::npos,
                        "legacy export changed format");
                require(codec.decode(bytes, view) == codec_status::ok && view->header().expires_at_ms == expiry,
                        "legacy expiry conversion failed");
            } else {
                bytes = "unchanged";
                require(legacy.encode(record, bytes) == codec_status::unsupported_precision && bytes == "unchanged",
                        "legacy export silently lost millisecond precision");
            }
        }
    }
    std::unique_ptr<record_view> view;
    const std::vector<std::string> invalid = {
        "", "[]", "{}", "{\"T\":\"S\",\"E\":-1,\"E\":0,\"D\":\"x\"}",
        "{\"T\":\"S\",\"E\":9223372036854775807,\"D\":\"x\"}",
        "{\"T\":\"S\",\"E\":-1,\"P\":-1,\"D\":\"x\"}",
        "{\"V\":1,\"T\":\"S\",\"E\":1,\"P\":1001,\"D\":\"x\"}",
        "{\"V\":1,\"T\":\"S\",\"E\":-1,\"P\":-2,\"D\":\"x\"}",
        "{\"V\":1,\"T\":\"S\",\"E\":-1,\"D\":\"x\"}",
        "{\"V\":1,\"T\":\"S\",\"E\":-1,\"P\":-1,\"D\":{}}"
    };
    for (const auto& bytes : invalid) require(codec.decode(bytes, view) == codec_status::invalid_record && !view,
                                              "invalid record accepted");
    require(codec.decode("{\"V\":99}", view) == codec_status::unsupported_version && !view, "future format accepted");
    require(codec.decode("{\"T\":\"H\",\"E\":-1,\"D\":{\"a\":5}}", view) == codec_status::ok, "lazy hash header failed");
    std::string value;
    std::map<std::string, std::string> fields;
    require(view->hash_field("a", value) == field_status::invalid && !view->hash_values(fields), "invalid hash field accepted");
    // Exercise the exact same DAOs against a non-JSON codec bound to the DB.
    auto db = db::create_mdb(4);
    require(db->open("", nullptr, std::make_shared<text_codec>()), "custom codec DB open failed");
    dao::json_string strings;
    dao::json_key keys;
    require(strings.set(db, binary, binary), "custom codec set failed");
    std::string bytes;
    require(db->get(binary, bytes) && bytes.compare(0, 5, "TEXT:") == 0, "DAO bypassed configured codec");
    require(strings.get(db, binary, value) && value == binary, "custom codec get failed");
    // Exercise custom-codec range fallbacks before native expiry migration.
    long long custom_length, custom_integer;
    require(strings.length(db, binary, custom_length) && custom_length == 256,
        "custom codec length fallback failed");
    require(strings.getrange(db, binary, -2, -1, value) &&
        value == binary.substr(254), "custom codec range fallback failed");
    dao::paged_string native_strings;
    require(keys.expire(db, binary, 60) &&
        native_strings.get(db, binary, value) && value == binary,
        "expiry custom-codec import failed");
    require(keys.persist(db, binary), "custom codec persist failed");
    long long ttl;
    require(keys.pttl(db, binary, ttl) && ttl == -1, "custom codec expiry was not removed");
    require(native_strings.length(db, binary, custom_length) &&
        custom_length == 256, "custom-codec imported length failed");
    require(native_strings.getrange(db, binary, -2, -1, value) &&
        value == binary.substr(254), "custom-codec imported range failed");
    require(strings.increment(db, "custom-counter", 1, false, custom_integer) && custom_integer == 1,
            "custom codec increment failed");
    require(strings.append(db, "custom-counter", "0", custom_length) && custom_length == 2, "custom codec append failed");
    require(strings.get(db, "custom-counter", value) && value == "10", "custom codec append bytes lost");
    require(strings.setrange(db, "custom-counter", 1, "2", custom_length) && custom_length == 2 &&
            strings.get(db, "custom-counter", value) && value == "12", "custom codec range write failed");
    bool batch_applied;
    require(strings.mset(db, {{"custom-a", binary}, {"custom-b", ""}}, false, batch_applied) && batch_applied,
            "batch SET bypassed custom codec");
    std::vector<dao::string_result> batch_values;
    require(strings.mget(db, {"custom-b", "custom-a", "custom-b"}, batch_values) && batch_values.size() == 3 &&
            batch_values[0].found && batch_values[0].value.empty() && batch_values[1].value == binary &&
            batch_values[2].value.empty(), "batch GET bypassed custom codec");
    require(db->set("future", "unrelated bytes"), "custom corrupt fixture");
    require(!strings.get(db, "future", value) && strings.status() == dao::dao_base::read_status::invalid_record,
            "custom codec failure hidden");
    require(db->get("future", value) && value == "unrelated bytes", "decoder failure deleted bytes");
    dao::json_hash hashes;
    require(hashes.hmset(db, "unsupported-hash", {{"field", "value"}}) == -1 &&
            hashes.status() == dao::dao_base::read_status::unsupported_format,
            "unsupported custom codec value was written");
    require(!db->get("unsupported-hash", value), "failed codec write created a key");
    auto current = db::create_mdb(4);
    require(current->open(""), "current codec DB open");
    const std::string future = "{\"V\":99,\"T\":\"S\",\"E\":0,\"D\":\"future\"}";
    require(current->set("future", future), "future record fixture");
    require(!strings.get(current, "future", value) && strings.status() == dao::dao_base::read_status::unsupported_format,
            "future record version was interpreted");
    require(!keys.expire(current, "future", 60) && current->get("future", value) && value == future,
            "future record was expired or rewritten");
    const long long legacy_deadline = unix_time_ms() / 1000 + 60;
    require(current->set("legacy-hash", "{\"T\":\"H\",\"E\":" + std::to_string(legacy_deadline) +
                         ",\"D\":{\"a\":\"one\"}}"), "legacy migration fixture");
    require(hashes.hmset(current, "legacy-hash", {{"b", "two"}}) == 1, "legacy hash mutation failed");
    require(current->get("legacy-hash", bytes) && bytes.find("\"V\":1") != std::string::npos,
            "legacy write did not use current codec");
    require(codec.decode(bytes, view) == codec_status::ok && view->header().expires_at_ms == legacy_deadline * 1000,
            "legacy migration changed deadline");
    require(view->hash_values(fields) && fields == std::map<std::string, std::string>{{"a", "one"}, {"b", "two"}},
            "legacy migration changed fields");
    auto selective = db::create_mdb(4);
    require(selective->open("", nullptr, std::make_shared<selective_codec>()), "selective codec open failed");
    value_record selected_record;
    selected_record.header.kind = value_kind::hash;
    selected_record.hash_value = {{binary, binary}, {"", ""}, {"unrequested", "untouched"}};
    std::string selected_bytes;
    require(codec.encode(selected_record, selected_bytes) == codec_status::ok && selective->set("hash", selected_bytes),
            "selective codec fixture failed");
    std::map<std::string, std::string> selected_fields;
    require(hashes.hmget(selective, "hash", {binary, "missing", "", binary}, selected_fields) &&
            selected_fields == std::map<std::string, std::string>{{binary, binary}, {"", ""}},
            "selective HMGET failed");
    bool exists;
    long long field_length;
    require(hashes.hstrlen(selective, "hash", binary, field_length, exists) && exists && field_length == 256,
            "codec default hash field length failed");
    require(hashes.hstrlen(selective, "hash", "", field_length, exists) && exists && field_length == 0,
            "codec default empty hash field length failed");
    require(hashes.hstrlen(selective, "hash", "missing", field_length, exists) && !exists && field_length == 0,
            "codec default missing hash field length failed");
    value_record list_record;
    list_record.header.kind = value_kind::list;
    list_record.list_value = {"", binary, "same", "same"};
    for (long long expiry : {-1LL, 0LL, 1000LL, 1001LL, LLONG_MAX}) {
        list_record.header.expires_at_ms = expiry;
        require(codec.encode(list_record, bytes) == codec_status::ok && codec.decode(bytes, view) == codec_status::ok &&
                view->header().kind == value_kind::list && view->header().expires_at_ms == expiry, "list codec header mismatch");
        bytes.clear(); // decoded views retain ownership
        std::vector<std::string> items;
        size_t size;
        require(view->list_size(size) && size == 4 && view->list_values(items) && items == list_record.list_value,
                "list codec binary roundtrip failed");
        require(view->list_range(1, -2, items) && items == std::vector<std::string>{binary, "same"}, "list codec range failed");
        require(view->list_range(LLONG_MIN, LLONG_MIN, items) && items.empty(), "list codec negative end failed");
        require(view->list_range(LLONG_MIN, LLONG_MAX, items) && items == list_record.list_value, "list codec extreme range failed");
    }
    list_record.header.expires_at_ms = -1;
    bytes = "unchanged";
    require(legacy.encode(list_record, bytes) == codec_status::unsupported_version && bytes == "unchanged",
            "list legacy export was silently accepted");
    vector_list_view fallback_list(list_record.header, list_record.list_value);
    std::vector<std::string> list_items;
    size_t list_size;
    require(fallback_list.list_size(list_size) && list_size == 4 && fallback_list.list_range(-3, -2, list_items) &&
            list_items == std::vector<std::string>{binary, "same"}, "default list accessors failed");
    list_search search;
    std::vector<long long> positions;
    search.rank = -1;
    search.count = 0;
    require(fallback_list.list_positions("same", search, positions) && positions == std::vector<long long>{3, 2},
            "default list position fallback failed");
    search.rank = -2;
    require(fallback_list.list_positions("same", search, positions) && positions == std::vector<long long>{2},
            "default negative rank failed");
    search.maxlen = 1;
    require(fallback_list.list_positions("same", search, positions) && positions.empty(), "default MAXLEN failed");
    search.rank = LLONG_MIN;
    require(!fallback_list.list_positions("same", search, positions), "default position search accepted invalid rank");
    list_record.list_value = {binary, "", binary, "end"};
    require(codec.encode(list_record, bytes) == codec_status::ok && codec.decode(bytes, view) == codec_status::ok,
            "position view fixture failed");
    search.rank = -1;
    search.maxlen = 0;
    require(view->list_positions(binary, search, positions) && positions == std::vector<long long>{2, 0}, "JSON binary reverse search failed");
    search.rank = 2;
    search.count = 1;
    require(view->list_positions(binary, search, positions) && positions == std::vector<long long>{2}, "JSON rank search failed");
    search.maxlen = 2;
    require(view->list_positions(binary, search, positions) && positions.empty(), "JSON MAXLEN cutoff failed");
    bytes = "unchanged";
    list_record.list_value.clear();
    require(codec.encode(list_record, bytes) == codec_status::invalid_record && bytes == "unchanged", "empty list encoding accepted");
    for (const auto* data : {"[]", "{}", "[5]", "[null]", "[[]]", "[\"ok\",5]"}) {
        const std::string record = std::string("{\"V\":1,\"T\":\"L\",\"E\":-1,\"P\":-1,\"D\":") + data + "}";
        require(codec.decode(record, view) == codec_status::invalid_record && !view, "invalid list payload accepted");
    }
    require(codec.decode("{\"T\":\"L\",\"E\":-1,\"D\":[\"x\"]}", view) == codec_status::unsupported_version && !view,
            "list was silently accepted as a legacy type");
    dao::json_list lists;
    long long list_length;
    require(!lists.push(db, "unsupported-list", {"x"}, true, false, list_length) &&
            lists.status() == dao::dao_base::read_status::unsupported_format && !db->get("unsupported-list", bytes),
            "old codec accepted an unsupported list write");
    selected_record.header.kind = value_kind::list;
    selected_record.list_value = {binary, "", binary};
    require(codec.encode(selected_record, selected_bytes) == codec_status::ok && selective->set("list", selected_bytes),
            "selective list fixture failed");
    search.rank = -1;
    search.count = 0;
    search.maxlen = 0;
    require(lists.positions(selective, "list", binary, search, positions) && positions == std::vector<long long>{2, 0},
            "selective LPOS failed");
    value_record set_record;
    set_record.header.kind = value_kind::set;
    set_record.set_value = {"", binary, "1", "01"};
    std::vector<long long> membership;
    std::set<std::string> set_members;
    for (long long expiry : {-1LL, 0LL, 1001LL, LLONG_MAX}) {
        set_record.header.expires_at_ms = expiry;
        require(codec.encode(set_record, bytes) == codec_status::ok && codec.decode(bytes, view) == codec_status::ok &&
                view->header().kind == value_kind::set && view->header().expires_at_ms == expiry, "Set codec header mismatch");
        bytes.clear();
        size_t size;
        require(view->set_size(size) && size == 4 && view->set_values(set_members) && set_members == set_record.set_value,
                "owning Set view lost binary members");
        require(view->set_membership({binary, "missing", "", binary, "1", "01"}, membership) &&
                membership == std::vector<long long>{1, 0, 1, 1, 1, 1}, "Set membership order mismatch");
    }
    set_record.header.expires_at_ms = -1;
    plain_set_view fallback_set(set_record.header, set_record.set_value);
    size_t set_size;
    require(fallback_set.set_size(set_size) && set_size == 4 && fallback_set.set_membership({"", "missing", binary}, membership) &&
            membership == std::vector<long long>{1, 0, 1}, "default Set accessor fallback failed");
    bytes = "unchanged";
    require(legacy.encode(set_record, bytes) == codec_status::unsupported_version && bytes == "unchanged", "legacy Set export accepted");
    for (const auto* payload : {"[]", "{}", "[5]", "[null]", "[[]]", "[\"a\",\"a\"]", "[\"a\",5]"}) {
        const auto record = std::string("{\"V\":1,\"T\":\"U\",\"E\":-1,\"P\":-1,\"D\":") + payload + "}";
        require(codec.decode(record, view) == codec_status::invalid_record && !view, "invalid Set payload accepted");
    }
    require(codec.decode("{\"T\":\"U\",\"E\":-1,\"D\":[\"a\"]}", view) == codec_status::unsupported_version && !view,
            "legacy Set type accepted");
    require(codec.encode(set_record, bytes) == codec_status::ok && selective->set("set", bytes), "selective Set fixture");
    dao::json_set sets;
    long long cardinality;
    require(sets.size(selective, "set", cardinality) && cardinality == 4 &&
            sets.membership(selective, "set", {binary, "missing", "", binary}, membership) &&
            membership == std::vector<long long>{1, 0, 1, 1}, "selective Set query failed");
    require(!sets.change(db, "unsupported-set", {"member"}, true, cardinality) &&
            sets.status() == dao::dao_base::read_status::unsupported_format && !db->get("unsupported-set", bytes),
            "unsupported custom codec accepted Set write");
    set_record.set_value.clear();
    bytes = "unchanged";
    require(codec.encode(set_record, bytes) == codec_status::invalid_record && bytes == "unchanged", "empty Set encoding accepted");
    const std::set<std::string> population = {"", binary, "a", "b"};
    std::mt19937_64 random(771921);
    std::vector<std::string> sampled;
    std::map<std::set<std::string>, int> pairs;
    for (int i = 0; i < 12000; ++i) {
        require(sample_set(population, 2, false, random, sampled) && sampled.size() == 2 && sampled[0] != sampled[1],
                "sampling without replacement returned duplicate/missing values");
        require(population.count(sampled[0]) && population.count(sampled[1]), "sample outside population");
        ++pairs[std::set<std::string>(sampled.begin(), sampled.end())];
    }
    require(pairs.size() == 6, "sampler failed to reach every unordered pair");
    for (const auto& pair : pairs) require(pair.second > 1600 && pair.second < 2400, "fixed-seed pair frequency grossly biased");
    require(sample_set(population, 20000, true, random, sampled) && sampled.size() == 20000, "replacement sample count mismatch");
    std::map<std::string, int> frequencies;
    for (const auto& member : sampled) {
        require(population.count(member) != 0, "replacement sample outside population");
        ++frequencies[member];
    }
    require(frequencies.size() == 4, "replacement sampler missed a member");
    for (const auto& frequency : frequencies) require(frequency.second > 4400 && frequency.second < 5600,
            "fixed-seed replacement frequency grossly biased");
    require(sample_set(population, ULLONG_MAX, false, random, sampled) && std::set<std::string>(sampled.begin(), sampled.end()) == population,
            "positive count did not clamp to population");
    require(!sample_set(population, ULLONG_MAX, true, random, sampled) && sampled.empty(), "unrepresentable sample was allocated");
    require(sample_set(std::set<std::string>(), ULLONG_MAX, true, random, sampled) && sampled.empty(), "empty population sampled");
    bool contains;
    require(fallback_set.set_contains(binary, contains) && contains && fallback_set.set_contains("missing", contains) && !contains,
            "default Set contains fallback failed");
    scan_options scanning;
    scanning.count = 1;
    scanning.no_values = true;
    scan_page page;
    std::set<std::string> scanned;
    do {
        require(fallback_set.set_scan(scanning, page), "default Set scan fallback failed");
        scanned.insert(page.values.begin(), page.values.end());
        scanning.cursor = page.cursor;
    } while (scanning.cursor);
    require(scanned == std::set<std::string>{"", binary, "1", "01"}, "default scan skipped members");
    dao::json_scan selective_scanner;
    scanning = scan_options();
    scanning.count = 1;
    scanning.no_values = true;
    scanned.clear();
    do {
        require(selective_scanner.open_collection(selective, "hash", value_kind::hash) &&
                selective_scanner.collection(scanning, page), "selective HSCAN materialized values");
        scanned.insert(page.values.begin(), page.values.end());
        scanning.cursor = page.cursor;
    } while (scanning.cursor);
    require(scanned == std::set<std::string>{"", binary, "unrequested"}, "selective HSCAN omitted fields");
    scanned.clear();
    do {
        require(selective_scanner.open_collection(selective, "set", value_kind::set) &&
                selective_scanner.collection(scanning, page), "selective SSCAN materialized members");
        scanned.insert(page.values.begin(), page.values.end());
        scanning.cursor = page.cursor;
    } while (scanning.cursor);
    require(scanned == std::set<std::string>{"", binary, "1", "01"}, "selective SSCAN omitted members");
    unsigned long long cursor;
    require(parse_scan_cursor("18446744073709551615", cursor) && cursor == ULLONG_MAX, "unsigned scan cursor truncated");
    require(!parse_scan_cursor("18446744073709551616", cursor) && !parse_scan_cursor("-1", cursor) &&
            !parse_scan_cursor(std::string("0\0suffix", 8), cursor), "invalid or binary-truncated cursor accepted");
    for (const auto* text : {"0", "00", "+0", "-0", " 0"}) require(parse_scan_cursor(text, cursor) && cursor == 0, "Redis cursor alias rejected");
    require(redis_glob_match(std::string("a\0*", 3), std::string("a\0binary", 8)) &&
            !redis_glob_match(std::string("a\0?", 3), std::string("a\0binary", 8)), "glob truncated NUL bytes");
    std::string deep_pattern, deep_value(3000, 'a');
    for (int i = 0; i < 3000; ++i) deep_pattern += "*a";
    require(redis_glob_match(deep_pattern, deep_value) && !redis_glob_match(deep_pattern + "b", deep_value), "glob deep pattern failed");
    // Independent dynamic-programming oracle for wildcard/literal/escape cases.
    std::mt19937 glob_random(102319);
    for (int trial = 0; trial < 1000; ++trial) {
        const std::string alphabet("ab*?\\\0", 6);
        std::string pattern, text;
        for (unsigned i = 0, size = glob_random() % 16; i < size; ++i) pattern.push_back(alphabet[glob_random() % alphabet.size()]);
        for (unsigned i = 0, size = glob_random() % 16; i < size; ++i) text.push_back(alphabet[glob_random() % alphabet.size()]);
        std::vector<bool> previous(text.size() + 1, false);
        previous[0] = true;
        for (size_t i = 0; i < pattern.size(); ++i) {
            char token = pattern[i];
            const bool escape = token == '\\';
            if (escape && i + 1 < pattern.size()) token = pattern[++i];
            std::vector<bool> current(text.size() + 1, false);
            if (!escape && token == '*') {
                current[0] = previous[0];
                for (size_t j = 1; j <= text.size(); ++j) current[j] = previous[j] || current[j - 1];
            } else {
                for (size_t j = 1; j <= text.size(); ++j) current[j] = previous[j - 1] && ((!escape && token == '?') || token == text[j - 1]);
            }
            previous.swap(current);
        }
        require(redis_glob_match(pattern, text) == previous.back(), "glob disagreed with independent wildcard oracle");
    }
    value_record sorted_record;
    sorted_record.header.kind = value_kind::zset;
    sorted_record.zset_value = {{"", -0.0}, {binary, 2}, {"tie", 2}, {"minimum", -std::numeric_limits<double>::infinity()},
                               {"maximum", std::numeric_limits<double>::infinity()}, {"tiny", std::numeric_limits<double>::denorm_min()}};
    for (long long expiry : {-1LL, 0LL, 1001LL, LLONG_MAX}) {
        sorted_record.header.expires_at_ms = expiry;
        require(codec.encode(sorted_record, bytes) == codec_status::ok && codec.decode(bytes, view) == codec_status::ok &&
                view->header().kind == value_kind::zset && view->header().expires_at_ms == expiry, "Sorted Set header roundtrip failed");
        bytes.clear();
        size_t size;
        std::vector<zset_score> scores;
        require(view->zset_size(size) && size == 6 && view->zset_scores({binary, "missing", "", binary}, scores) &&
                scores.size() == 4 && scores[0].found && scores[0].score == 2 && !scores[1].found &&
                scores[2].found && std::signbit(scores[2].score) && scores[3].score == 2, "Sorted Set scores or signed zero lost");
        std::vector<zset_member> range;
        require(view->zset_range(0, -1, false, range) && range.size() == 6 && range.front().member == "minimum" &&
                range.back().member == "maximum" && range[3].member == binary && range[4].member == "tie", "Sorted Set ordering mismatch");
        zset_rank rank;
        require(view->zset_position(binary, false, rank) && rank.found && rank.rank == 3 && rank.score == 2,
                "Sorted Set rank mismatch");
        require(view->zset_range(-2, -1, true, range) && range.size() == 2 && range[0].member == "" && range[1].member == "minimum",
                "Sorted Set reverse range mismatch");
    }
    sorted_record.header.expires_at_ms = -1;
    plain_zset_view fallback_sorted(sorted_record.header, sorted_record.zset_value);
    std::vector<zset_member> sorted_range;
    zset_rank sorted_rank;
    require(fallback_sorted.zset_range(0, 0, true, sorted_range) && sorted_range.size() == 1 && sorted_range[0].member == "maximum" &&
            fallback_sorted.zset_position(binary, true, sorted_rank) && sorted_rank.found && sorted_rank.rank == 2,
            "custom Sorted Set view fallback failed");
    // Compare the indexed owning view with a materializing custom codec across
    // empty/reversed intervals, binary bounds and extreme pagination values.
    require(codec.encode(sorted_record, bytes) == codec_status::ok && codec.decode(bytes, view) == codec_status::ok, "range codec fixture failed");
    for (auto mode : {zrange_mode::score, zrange_mode::lex})
        for (bool reverse : {false, true})
            for (long long offset : {-1LL, 0LL, 1LL, LLONG_MAX})
                for (long long limit : {LLONG_MIN, -1LL, 0LL, 1LL, LLONG_MAX})
                    for (bool exclusive : {false, true}) {
                        zset_query query;
                        query.mode = mode; query.reverse = reverse; query.offset = offset; query.limit = limit;
                        query.min_score.value = -0.0; query.max_score.value = 2;
                        query.min_score.exclusive = query.max_score.exclusive = exclusive;
                        query.min_lex.value = ""; query.max_lex.value = binary;
                        query.min_lex.exclusive = query.max_lex.exclusive = exclusive;
                        std::vector<zset_member> indexed, fallback;
                        long long indexed_count, fallback_count;
                        require(view->zset_select(query, indexed) && fallback_sorted.zset_select(query, fallback) && indexed.size() == fallback.size() &&
                                view->zset_count(query, indexed_count) && fallback_sorted.zset_count(query, fallback_count) &&
                                indexed_count == fallback_count && indexed_count == static_cast<long long>(indexed.size()), "range accessor/count fallback mismatch");
                        for (size_t i = 0; i < indexed.size(); ++i)
                            require(indexed[i].member == fallback[i].member && indexed[i].score == fallback[i].score, "indexed range selected wrong value");
                    }
    require(codec.encode(sorted_record, bytes) == codec_status::ok && selective->set("zscan", bytes), "selective ZSCAN fixture failed");
    for (bool fallback : {false, true}) {
        scan_options options;
        options.count = 1;
        std::map<std::string, std::string> found;
        size_t pages = 0;
        do {
            scan_page result;
            if (fallback) require(fallback_sorted.zset_scan(options, result), "default ZSCAN fallback failed");
            else require(selective_scanner.open_collection(selective, "zscan", value_kind::zset) &&
                         selective_scanner.collection(options, result), "selective ZSCAN requested full materialization");
            require(result.values.size() % 2 == 0 && ++pages <= 256, "ZSCAN split a score pair or failed to advance");
            for (size_t i = 0; i < result.values.size(); i += 2)
                require(found.emplace(result.values[i], result.values[i + 1]).second, "stable ZSCAN returned a duplicate member");
            options.cursor = result.cursor;
        } while (options.cursor);
        require(found.size() == sorted_record.zset_value.size() && pages > 1, "ZSCAN omitted members or ignored small hint");
        for (const auto& member : sorted_record.zset_value)
            require(found.at(member.first) == format_redis_double(member.second), "ZSCAN changed score precision or member bytes");
    }
    auto bad_scan_codec = std::make_shared<selective_codec>();
    bad_scan_codec->fail_zscan = true;
    auto bad_scan_db = db::create_mdb(1);
    require(bad_scan_db->open("", nullptr, bad_scan_codec) && bad_scan_db->set("zscan", bytes) &&
            selective_scanner.open_collection(bad_scan_db, "zscan", value_kind::zset) &&
            !selective_scanner.collection(scan_options(), page) && page.cursor == 0 && page.values.empty() &&
            selective_scanner.status() == dao::dao_base::read_status::invalid_record,
            "failed custom ZSCAN leaked partial member/score output");
    dao::json_zset sampler;
    dao::zset_sample_result picked;
    require(sampler.sample(selective, "zscan", 99, picked) && picked.found && picked.values.size() == sorted_record.zset_value.size(),
            "selective sample materialized all members");
    for (const auto& member : picked.values)
        require(sorted_record.zset_value.at(member.member) == member.score, "selective sample lost score");
    require(!sampler.sample(bad_scan_db, "zscan", 1, picked) && !picked.found && picked.values.empty() &&
            sampler.status() == dao::dao_base::read_status::invalid_record, "failed sample accessor leaked partial output");
    require(sampler.sample(bad_scan_db, "zscan", 0, picked) && picked.found && picked.values.empty(), "zero sample called failing accessor");
    require(fallback_sorted.zset_sample(99, false, picked.values) && picked.values.size() == sorted_record.zset_value.size(),
            "custom Sorted Set sampler fallback failed");
    std::map<std::string, double> sample_population;
    int sample_score = 0;
    for (const auto& member : population) sample_population[member] = ++sample_score;
    std::mt19937_64 set_random(924100), zset_random(924100);
    for (int iteration = 0; iteration < 1000; ++iteration) {
        const bool repeat = iteration % 2;
        const auto wanted = static_cast<unsigned long long>(iteration % 13);
        std::vector<std::string> set_result;
        std::vector<zset_member> zset_result;
        require(sample_set(population, wanted, repeat, set_random, set_result) &&
                sample_zset(sample_population, wanted, repeat, zset_random, zset_result) && set_result.size() == zset_result.size(),
                "generalized sampler changed Set or Sorted Set selection sizes");
        for (size_t i = 0; i < set_result.size(); ++i)
            require(set_result[i] == zset_result[i].member && zset_result[i].score == sample_population.at(set_result[i]),
                    "shared sampler selected different indexes or lost associated score");
    }
    require(!sample_zset(sample_population, ULLONG_MAX, true, zset_random, picked.values) && picked.values.empty(),
            "unrepresentable Sorted Set sample was allocated");
    require(sets.intersection_size(selective, {"set", "set"}, 1, cardinality) && cardinality == 1,
            "selective SINTERCARD copied the candidate collection or ignored LIMIT");
    const auto default_algebra = [](dao::zset_algebra_options&) { return true; };
    std::vector<zset_member> algebra_result;
    long long algebra_count;
    require(sampler.algebra(selective, {"set", "zscan", "zscan"}, dao::zset_operation::intersect, nullptr, false,
            default_algebra, algebra_result, algebra_count) && algebra_count == 2 && algebra_result.size() == 2 &&
            algebra_result[0].member.empty() && algebra_result[0].score == 1 && algebra_result[1].member == binary && algebra_result[1].score == 5,
            "selective intersection materialized full collections or deduplicated weighted contributions");
    require(sampler.algebra(selective, {"set", "zscan"}, dao::zset_operation::intersect, nullptr, true,
            [](dao::zset_algebra_options& options) { options.limit = 1; return true; }, algebra_result, algebra_count) &&
            algebra_count == 1 && algebra_result.empty(), "selective ZINTERCARD constructed a result or ignored LIMIT");
    require(!sampler.algebra(bad_scan_db, {"zscan"}, dao::zset_operation::unite, nullptr, false, default_algebra,
            algebra_result, algebra_count) && algebra_result.empty() && algebra_count == 0 &&
            sampler.status() == dao::dao_base::read_status::invalid_record, "failed member visitor leaked partial algebra output");
    require(selective->get("set", selected_bytes) && bad_scan_db->set("set", selected_bytes) &&
            !sampler.algebra(bad_scan_db, {"set", "zscan"}, dao::zset_operation::intersect, nullptr, true, default_algebra,
                algebra_result, algebra_count) && algebra_result.empty() && algebra_count == 0 &&
            sampler.status() == dao::dao_base::read_status::invalid_record, "failed single-score lookup leaked cardinality");
    // JSON owning traversal and custom materializing fallbacks stop successfully
    // after one callback, including when that callback requests early exit.
    for (record_view* visitor : {view.get(), static_cast<record_view*>(&fallback_sorted)}) {
        size_t visits = 0;
        require(visitor->zset_visit([&](const std::string& member, double score) {
            ++visits; require(sorted_record.zset_value.at(member) == score, "visitor changed member/score association"); return false;
        }) && visits == 1, "Sorted Set visitor ignored early termination");
        zset_score lookup;
        require(visitor->zset_lookup(binary, lookup) && lookup.found && lookup.score == 2 &&
                visitor->zset_lookup("not-present", lookup) && !lookup.found, "single-member score fallback retained stale output");
    }
    size_t set_visits = 0;
    require(fallback_set.set_visit([&](const std::string&) { ++set_visits; return false; }) && set_visits == 1,
            "Set visitor fallback ignored early termination");
    scan_options no_match;
    no_match.has_match = true; no_match.pattern = "no-such-member"; no_match.count = 1;
    require(view->zset_scan(no_match, page) && page.values.empty() && page.cursor != 0, "filtered ZSCAN lost continuation");
    no_match.cursor = ULLONG_MAX;
    require(view->zset_scan(no_match, page) && page.values.empty() && page.cursor == 0, "out-of-range ZSCAN cursor did not finish");
    bytes = "unchanged";
    require(legacy.encode(sorted_record, bytes) == codec_status::unsupported_version && bytes == "unchanged", "legacy Sorted Set export accepted");
    for (const auto* payload : {"[]", "{}", "[5]", "[[\"a\",1]]", "[[\"a\",\"nan\"]]", "[[\"a\",\"1e309\"]]", "[[\"a\"]]",
                               "[[\"a\",\"1\"],[\"a\",\"2\"]]"}) {
        const auto record = std::string("{\"V\":1,\"T\":\"Z\",\"E\":-1,\"P\":-1,\"D\":") + payload + "}";
        require(codec.decode(record, view) == codec_status::invalid_record && !view, "invalid Sorted Set payload accepted");
    }
    require(codec.decode("{\"T\":\"Z\",\"E\":-1,\"D\":[[\"a\",\"1\"]]}", view) == codec_status::unsupported_version && !view,
            "legacy Sorted Set type accepted");
    sorted_record.zset_value.clear();
    require(codec.encode(sorted_record, bytes) == codec_status::invalid_record && bytes == "unchanged", "empty Sorted Set persisted");
    sorted_record.zset_value["nan"] = std::numeric_limits<double>::quiet_NaN();
    require(codec.encode(sorted_record, bytes) == codec_status::invalid_record && bytes == "unchanged", "NaN score persisted");
    double score;
    for (const auto& text : {std::string("nan"), std::string(" 1"), std::string("1 "), std::string("1e309"),
                              std::string("1e-9999"), std::string("1\0suffix", 8)})
        require(!parse_redis_double(text, score), "invalid score text accepted");
    require(parse_redis_double("0x1p+2", score) && score == 4 && format_redis_double(-0.0) == "-0", "hex score or negative zero failed");
    require(parse_redis_double("-000", score) && score == 0 &&
        std::signbit(score), "short score lost negative zero");
    require(parse_redis_score_endpoint("+999999999999999", score) &&
        score == 999999999999999.0, "short integer endpoint changed value");
    require(parse_redis_double("9007199254740993", score) &&
        score == 9007199254740992.0, "large integer fallback changed rounding");
    std::mt19937_64 score_random(993812);
    for (int i = 0; i < 10000; ++i) {
        const auto bits = score_random();
        double original, restored;
        std::memcpy(&original, &bits, sizeof(original));
        if (std::isnan(original)) continue;
        require(parse_redis_double(format_redis_double(original), restored) &&
                std::memcmp(&original, &restored, sizeof(original)) == 0, "double formatter lost score precision");
    }
    const std::string previous_numeric_locale = std::setlocale(LC_NUMERIC, nullptr);
    if (std::setlocale(LC_NUMERIC, "fr_FR.UTF-8")) {
        require(parse_redis_double("1.5", score) && score == 1.5 && !parse_redis_double("1,5", score) &&
                format_redis_double(1.5) == "1.5", "score parser/formatter followed process numeric locale");
        std::cout << "Score parsing verified under fr_FR.UTF-8 numeric locale\n";
    }
    require(std::setlocale(LC_NUMERIC, previous_numeric_locale.c_str()) != nullptr, "numeric locale restoration failed");
    value_record concurrent_sorted;
    concurrent_sorted.header.kind = value_kind::zset;
    for (int i = 0; i < 250; ++i) concurrent_sorted.zset_value["member:" + std::to_string(i)] = i % 7;
    require(codec.encode(concurrent_sorted, bytes) == codec_status::ok && codec.decode(bytes, view) == codec_status::ok,
            "concurrent ordered view fixture failed");
    const auto expected_order = ordered_zset(concurrent_sorted.zset_value);
    std::vector<std::thread> readers;
    for (int thread = 0; thread < 4; ++thread) readers.emplace_back([&] {
        for (int iteration = 0; iteration < 100; ++iteration) {
            std::vector<zset_member> range;
            zset_rank rank;
            scan_page shared_page;
            require(view->zset_scan(scan_options(), shared_page) && shared_page.values.size() % 2 == 0, "shared ZSCAN split a pair");
            for (size_t i = 0; i < shared_page.values.size(); i += 2) {
                const auto found = concurrent_sorted.zset_value.find(shared_page.values[i]);
                require(found != concurrent_sorted.zset_value.end() && shared_page.values[i + 1] == format_redis_double(found->second),
                        "shared ZSCAN returned incomplete data");
            }
            require(view->zset_range(0, -1, false, range) && range.size() == expected_order.size(), "shared ordered view omitted entries");
            for (size_t i = 0; i < range.size(); ++i) require(range[i].member == expected_order[i].member &&
                    range[i].score == expected_order[i].score, "shared ordered view published an incomplete index");
            require(view->zset_position(expected_order.back().member, false, rank) && rank.found && rank.rank == 249,
                    "shared ordered view rank mismatch");
        }
    });
    for (auto& reader : readers) reader.join();
    dao::json_zset sorted;
    dao::zadd_options sorted_options;
    dao::zadd_result sorted_result;
    require(!sorted.add(db, "unsupported-sorted", {{"member", 1}}, sorted_options, sorted_result) &&
            sorted.status() == dao::dao_base::read_status::unsupported_format && !db->get("unsupported-sorted", bytes),
            "custom codec silently accepted unsupported Sorted Set");
    bool renamed;
    std::string rename_bytes, renamed_bytes;
    require(selective->get("hash", rename_bytes) && keys.rename(selective, "hash", "renamed-hash", false, renamed) && renamed &&
            !selective->get("hash", renamed_bytes) && selective->get("renamed-hash", renamed_bytes) && renamed_bytes == rename_bytes,
            "opaque rename encoded a read-only codec or materialized its fields");
    require(db->set("rename-text", "TEXT:-1\ncustom") && keys.rename(db, "rename-text", "renamed-text", false, renamed) && renamed &&
            db->get("renamed-text", renamed_bytes) && renamed_bytes == "TEXT:-1\ncustom",
            "non-JSON payload validation fallback or byte-preserving rename failed");
    auto copy_json = db::create_mdb(2);
    require(copy_json->open("") && db->set("copy-text", "TEXT:4102444800123\n" + binary), "cross-codec COPY fixture failed");
    bool copied;
    require(keys.copy(db, "copy-text", copy_json, "copy-json", true, copied) && copied &&
            strings.get(copy_json, "copy-json", renamed_bytes) && renamed_bytes == binary &&
            keys.expiry_time(copy_json, "copy-json", algebra_count) && algebra_count == 4102444800123LL,
            "COPY failed text-to-JSON conversion or changed expiry");
    require(keys.copy(copy_json, "copy-json", db, "copy-text-back", true,
        copied) && copied &&
        native_strings.get(db, "copy-text-back", renamed_bytes) &&
        renamed_bytes == binary &&
        keys.expiry_time(db, "copy-text-back", algebra_count) &&
        algebra_count == 4102444800123LL,
        "native COPY into text-codec DB changed payload/expiry");
    require(keys.copy(selective, "renamed-hash", selective, "copied-readonly", true, copied) && copied &&
            selective->get("copied-readonly", renamed_bytes) && renamed_bytes == rename_bytes,
            "same-codec COPY unnecessarily encoded a read-only codec");
    std::cout << "Codec regression tests passed\n";
}
