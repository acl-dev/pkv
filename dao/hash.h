#pragma once
#include "db/db.h"
#include "dao_base.h"

namespace pkv {
namespace dao {

struct hash_field_value {
    bool found = false;
    std::string value;
};
using hash_field_values = std::vector<hash_field_value>;
// Runs after type validation under the key lock. Only parse arguments; do not
// perform I/O, retain borrowed state or reenter the database.
using hash_fields_parser = std::function<bool(std::vector<std::string>&)>;


// This class represents a hash data structure and inherits from dao_base class.
class hash : public dao_base {
public:
    hash() = default;
    virtual ~hash() override = default;

    virtual bool hgetdel(shared_db&, const std::string&,
        const hash_fields_parser&, hash_field_values& out) {
        out.clear(); read_status_ = read_status::unsupported_operation;
        return false;
    }

    // Deletes a field from a hash stored at key.
    virtual int hdel(shared_db& db, const std::string& key, const std::string& name) = 0;

    virtual int hdel(shared_db& db, const std::string& key, const std::vector<std::string>& names) = 0;

    // Sets the string value of a hash field.
    virtual bool hset(shared_db& db, const std::string& key, const std::string& name,
            const std::string& value) = 0;

    // Gets the value associated with field in the hash stored at key.
    virtual bool hget(shared_db& db, const std::string& key, const std::string& name,
            std::string& value) = 0;

    // Gets all fields and values in a hash stored at key.
    virtual bool hgetall(shared_db& db, const std::string& key) = 0;

    // Legacy implementations retain the existing materializing fallback.
    virtual bool hgetall(shared_db& db, const std::string& key,
        bool with_values) {
        (void) with_values;
        return hgetall(db, key);
    }

    // Sets multiple hash fields to multiple values.
    virtual int hmset(shared_db& db, const std::string& key,
            const std::map<std::string, std::string>& fields) = 0;

    // Gets the values of all the given hash fields.
    virtual bool hmget(shared_db& db, const std::string& key,
            const std::vector<std::string>& names,
            std::map<std::string, std::string>& result) = 0;

    virtual bool hsetnx(shared_db& db, const std::string& key, const std::string& name,
            const std::string& value, bool& added) = 0;
    virtual bool hincrby(shared_db& db, const std::string& key, const std::string& name,
            long long delta, long long& value) = 0;
    virtual bool hlen(shared_db& db, const std::string& key, long long& length) = 0;
    virtual bool hstrlen(shared_db& db, const std::string& key, const std::string& name,
            long long& length, bool& exists) = 0;

    // Returns all fields and values of the hash stored at key.
    virtual const std::map<std::string, std::string>& get_fields() = 0;

    using sample_values = std::vector<std::pair<std::string, std::string>>;
    virtual bool hincrbyfloat(shared_db&, const std::string&,
        const std::string&, const std::string&, std::string& out) {
        out.clear(); read_status_ = read_status::unsupported_operation;
        return false;
    }
    virtual bool sample(shared_db&, const std::string&, long long, bool,
        sample_values& out) {
        out.clear(); read_status_ = read_status::unsupported_operation;
        return false;
    }
};

} // namespace dao
} // namespace pkv
