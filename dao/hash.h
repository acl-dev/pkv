#pragma once
#include "db/db.h"
#include "dao_base.h"

namespace pkv {
namespace dao {

class hash : public dao_base {
public:
    hash() = default;
    virtual ~hash() override = default;

    virtual int hdel(shared_db& db, const std::string& key, const std::string& name) = 0;
    virtual bool hset(shared_db& db, const std::string& key, const std::string& name,
            const std::string& value) = 0;
    virtual bool hget(shared_db& db, const std::string& key, const std::string& name,
            std::string& value) = 0;
    virtual bool hgetall(shared_db& db, const std::string& key) = 0;
    virtual int hmset(shared_db& db, const std::string& key,
            const std::map<std::string, std::string>& fields) = 0;
    virtual bool hmget(shared_db& db, const std::string& key,
            const std::vector<std::string>& names,
            std::map<std::string, std::string>& result) = 0;
    virtual const std::map<std::string, std::string>& get_fields() = 0;
};

} // namespace dao
} // namespace pkv
