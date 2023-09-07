#pragma once
#include "db/db.h"
#include "dao_base.h"

namespace pkv {
namespace dao {


// This class represents a hash data structure and inherits from dao_base class.
class hash : public dao_base {
public:
    hash() = default;
    virtual ~hash() override = default;

    // Deletes a field from a hash stored at key.
    virtual int hdel(shared_db& db, const std::string& key, const std::string& name) = 0;

    // Sets the string value of a hash field.
    virtual bool hset(shared_db& db, const std::string& key, const std::string& name,
            const std::string& value) = 0;

    // Gets the value associated with field in the hash stored at key.
    virtual bool hget(shared_db& db, const std::string& key, const std::string& name,
            std::string& value) = 0;

    // Gets all fields and values in a hash stored at key.
    virtual bool hgetall(shared_db& db, const std::string& key) = 0;

    // Sets multiple hash fields to multiple values.
    virtual int hmset(shared_db& db, const std::string& key,
            const std::map<std::string, std::string>& fields) = 0;

    // Gets the values of all the given hash fields.
    virtual bool hmget(shared_db& db, const std::string& key,
            const std::vector<std::string>& names,
            std::map<std::string, std::string>& result) = 0;

    // Returns all fields and values of the hash stored at key.
    virtual const std::map<std::string, std::string>& get_fields() = 0;
};

} // namespace dao
} // namespace pkv
