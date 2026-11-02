#pragma once
#include "hash.h"
#include "map_status.h"

namespace pkv {
namespace dao {

class paged_hash : public hash {
public:
    bool hgetdel(shared_db& db, const std::string& key,
        const hash_fields_parser& parse, hash_field_values& out) override;
    int hdel(shared_db& db, const std::string& key,
        const std::string& name) override;
    int hdel(shared_db& db, const std::string& key,
        const std::vector<std::string>& names) override;
    bool hset(shared_db& db, const std::string& key, const std::string& name,
        const std::string& value) override;
    bool hget(shared_db& db, const std::string& key, const std::string& name,
        std::string& value) override;
    bool hgetall(shared_db& db, const std::string& key) override;
    bool hgetall(shared_db& db, const std::string& key,
        bool with_values) override;
    int hmset(shared_db& db, const std::string& key,
        const std::map<std::string, std::string>& fields) override;
    bool hmget(shared_db& db, const std::string& key,
        const std::vector<std::string>& names,
        std::map<std::string, std::string>& result) override;
    bool hsetnx(shared_db& db, const std::string& key, const std::string& name,
        const std::string& value, bool& added) override;
    bool hincrby(shared_db& db, const std::string& key, const std::string& name,
        long long delta, long long& value) override;
    bool hincrbyfloat(shared_db& db, const std::string& key,
        const std::string& name, const std::string& delta,
        std::string& value) override;
    bool hlen(shared_db& db, const std::string& key,
        long long& length) override;
    bool hstrlen(shared_db& db, const std::string& key, const std::string& name,
        long long& length, bool& exists) override;
    bool sample(shared_db& db, const std::string& key, long long count,
        bool with_values, sample_values& out) override;
    const std::map<std::string, std::string>& get_fields() override {
        return fields_;
    }
private:
    std::map<std::string, std::string> fields_;
    using operation = std::function<bool(map_store&)>;
    bool run(shared_db& db, const std::string& key, const operation& callback);
    bool open(shared_db& db, page_transaction& transaction,
        const std::string& key, map_store& store);
    bool import(map_store& store);
    bool store_result(map_store& store);
    bool choose(map_store& store, long long count, bool with_values,
        sample_values& out);
};

} // namespace dao
} // namespace pkv
