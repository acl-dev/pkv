#pragma once
#include "string.h"
#include "string_status.h"

namespace pkv {
namespace dao {
class paged_string : public string {
public:
    bool set(shared_db& db, const std::string& key, const char* data) override;
    bool set(shared_db& db, const std::string& key,
        const std::string& data) override;
    bool set(shared_db& db, const std::string& key, const std::string& data,
        const set_options& options, set_result& result) override;
    bool get(shared_db& db, const std::string& key, std::string& out) override;
    bool digest(shared_db& db, const std::string& key,
        string_result& out) override;
    bool delex(shared_db& db, const std::string& key,
        const set_options& options, long long& removed) override;
    bool mget(shared_db& db, const std::vector<std::string>& keys,
        std::vector<string_result>& values) override;
    bool mset(shared_db& db, const std::map<std::string, std::string>& values,
        bool only_missing, bool& applied) override;
    bool msetex(shared_db& db, const string_assignments& values,
        const set_options& options, bool& applied) override;
    bool increment(shared_db& db, const std::string& key, long long delta,
        bool subtract, long long& value) override;
    bool increment_float(shared_db& db, const std::string& key,
        const std::string& delta, std::string& value) override;
    bool append(shared_db& db, const std::string& key,
        const std::string& suffix, long long& length) override;
    bool setrange(shared_db& db, const std::string& key, long long offset,
        const std::string& value, long long& length) override;
    bool length(shared_db& db, const std::string& key,
        long long& length) override;
    bool getrange(shared_db& db, const std::string& key, long long start,
        long long end, std::string& out) override;
    bool getex(shared_db& db, const std::string& key,
        const get_options& options, string_result& result) override;
    bool getbit(shared_db& db, const std::string& key, long long offset,
        long long& bit) override;
    bool setbit(shared_db& db, const std::string& key, long long offset,
        int bit, long long& previous) override;
    bool bitcount(shared_db& db, const std::string& key,
        const bitmap_range& range, long long& count) override;
    bool bitpos(shared_db& db, const std::string& key, int bit,
        const bitmap_range& range, long long& position) override;
    bool bitop(shared_db& db, bitmap_operation operation,
        const std::vector<std::string>& sources,
        const std::string& destination, long long& length) override;
    bool bitfield(shared_db& db, const std::string& key,
        const std::vector<bitfield_operation>& operations, bool readonly,
        std::vector<bitfield_reply>& out) override;
    bool pfadd(shared_db& db, const std::string& key,
        const std::vector<const std::string*>& elements, long long& changed);
    bool pfcount(shared_db& db, const std::vector<std::string>& keys,
        long long& count);
    bool pfmerge(shared_db& db, const std::vector<std::string>& keys);
    bool lcs(shared_db& db, const std::string& first,
        const std::string& second,
        const std::vector<const std::string*>& arguments, lcs_options& options,
        lcs_result& out, const char*& error) override;
private:
    bool pfaggregate(shared_db& db, const std::vector<std::string>& keys,
        bool write, long long& count);
    bool hll_value(string_store& store, std::string& value, bool body);
    bool digest_current(page_transaction& tx, const std::string& key,
        std::string& out);
    bool comparison(page_transaction& tx, const std::string& key,
        const set_options& options, const std::string* old, bool& equal);
    bool valid_bit_offset(long long offset);
    long long now_ = 0;
    using operation = std::function<bool(string_store&)>;
    bool run(shared_db& db, const std::string& key, const operation& callback);
    bool open(shared_db& db, page_transaction& tx, const std::string& key,
        string_store& store);
    bool import(string_store& store);
    bool stored(string_store& store);
    bool result(db::batch_status status);
    bool matches(page_transaction& tx, const std::string& key,
        const std::string& expected, bool& equal);
    bool current(shared_db& db, page_transaction& tx, const std::string& key,
        std::string& out);
};
} // namespace dao
} // namespace pkv
