#pragma once
#include "db/db.h"
#include "../hash.h"

namespace pkv {
namespace dao {

class json_hash : public hash {
public:
    json_hash() = default;
    ~json_hash() override = default;

    // @override
    int hdel(shared_db& db, const std::string& key, const std::string& name) override;

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

    // @override
    const std::map<std::string, std::string>& get_fields() override {
        return fields_;
    }

private:
    std::map<std::string, std::string> fields_;

    const char* build();
};

} // namespace dao
} // namespace pkv
