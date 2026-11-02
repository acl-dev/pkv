#pragma once
#include "set.h"
#include "map_status.h"

namespace pkv {
namespace dao {

class paged_set : public set {
public:
    bool change(shared_db& db, const std::string& key,
        const std::vector<std::string>& members, bool add,
        long long& changed) override;
    bool size(shared_db& db, const std::string& key,
        long long& count) override;
    bool membership(shared_db& db, const std::string& key,
        const std::vector<std::string>& members,
        std::vector<long long>& out) override;
    bool members(shared_db& db, const std::string& key,
        std::set<std::string>& out) override;
    bool move(shared_db& db, const std::string& source,
        const std::string& destination, const std::string& member,
        bool& moved) override;
    bool algebra(shared_db& db, const std::vector<std::string>& keys,
        set_operation operation, const std::string* destination,
        std::set<std::string>& out) override;
    bool intersection_size(shared_db& db,
        const std::vector<std::string>& keys, long long limit,
        long long& count) override;
    bool sample(shared_db& db, const std::string& key, long long count,
        bool remove, set_sample_result& out) override;
private:
    using operation = std::function<bool(map_store&)>;
    using stores = std::map<std::string, std::unique_ptr<map_store>>;
    bool run(shared_db& db, const std::string& key,
        const operation& callback);
    bool open(shared_db& db, page_transaction& tx, const std::string& key,
        map_store& store, long long now);
    bool import(map_store& store);
    bool result(db::batch_status status);
    bool finish(map_store& store);
    bool sources(shared_db& db, page_transaction& tx,
        const std::vector<std::string>& keys, stores& out);
    bool contains(map_store& store, const std::string& member, bool& found);
    bool accept(stores& sources, const std::string& first,
        const std::string& member, set_operation operation, bool& accepted);
    bool choose(map_store& store, long long count,
        std::vector<std::string>& out);
};

} // namespace dao
} // namespace pkv
