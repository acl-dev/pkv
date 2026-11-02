#pragma once
#include "db/db.h"
#include "../hash.h"

namespace pkv {
namespace dao {

class json_hash : public hash {
public:
    bool hgetdel(shared_db& db, const std::string& key,
        const hash_fields_parser& parse, hash_field_values& out) override;
    json_hash() = default;
    ~json_hash() override = default;

    // @override
    int hdel(shared_db& db, const std::string& key, const std::string& name) override;

    int hdel(shared_db& db, const std::string& key, const std::vector<std::string>& names) override;

    // @override
    bool hset(shared_db& db, const std::string& key, const std::string& name,
            const std::string& value) override;

    // @override
    bool hget(shared_db& db, const std::string& key, const std::string& name,
            std::string& value) override;

    // @override
    bool hgetall(shared_db& db, const std::string& key) override;

    // @override
    int hmset(shared_db& db, const std::string& key,
            const std::map<std::string, std::string>& fields) override;

    // @override
    bool hmget(shared_db& db, const std::string& key,
            const std::vector<std::string>& names,
            std::map<std::string, std::string>& result) override;

    virtual bool hsetnx(shared_db& db, const std::string& key, const std::string& name,
            const std::string& value, bool& added) override;
    virtual bool hincrby(shared_db& db, const std::string& key, const std::string& name,
            long long delta, long long& value) override;
    virtual bool hlen(shared_db& db, const std::string& key, long long& length) override;
    virtual bool hstrlen(shared_db& db, const std::string& key, const std::string& name,
            long long& length, bool& exists) override;

    // @override
    const std::map<std::string, std::string>& get_fields() override {
        return record_.hash_value;
    }

private:
    value_record record_;
    bool parse_fields();
};

} // namespace dao
} // namespace pkv
