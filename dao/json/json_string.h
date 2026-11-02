#pragma once
#include "db/db.h"
#include "../string.h"

namespace pkv {
namespace dao {

class json_string : public string {
public:
    json_string();
    ~json_string() override = default;

    // @override
    bool set(shared_db& db, const std::string& key, const char* data) override;

    bool set(shared_db& db, const std::string& key, const std::string& data) override;

    bool set(shared_db& db, const std::string& key, const std::string& data,
             const set_options& options, set_result& result) override;

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
    bool increment(shared_db& db, const std::string& key, long long delta, bool subtract, long long& value) override;
    bool increment_float(shared_db& db, const std::string& key,
        const std::string& delta, std::string& value) override;
    bool append(shared_db& db, const std::string& key, const std::string& suffix, long long& length) override;
    bool setrange(shared_db& db, const std::string& key, long long offset, const std::string& value, long long& length) override;
    bool length(shared_db& db, const std::string& key, long long& length) override;
    bool getrange(shared_db& db, const std::string& key, long long start, long long end, std::string& out) override;
    bool getex(shared_db& db, const std::string& key, const get_options& options, string_result& result) override;

    // @override
    bool get(shared_db& db, const std::string& key, std::string& out) override;

public:
    void set_string(const char* data);
    bool to_string(std::string& out);

private:
    bool comparison(const set_options& options, const std::string* old,
        bool& equal);
    using mutator = std::function<bool(bool, value_record&, long long, db::update_action&)>;
    bool mutate(shared_db& db, const std::string& key, const mutator& callback);
    std::string data_;
};

} // namespace dao
} // namespace pkv
