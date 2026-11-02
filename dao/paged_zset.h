#pragma once
#include "zset.h"
#include "db/zset_store.h"
#include "map_status.h"
#include "common/geo_search.h"

namespace pkv {
namespace dao {

class paged_zset : public zset {
public:
    bool add(shared_db& db, const std::string& key,
        const std::vector<zset_member>& values, const zadd_options& options,
        zadd_result& out) override;
    bool remove(shared_db& db, const std::string& key,
        const std::vector<std::string>& values, long long& removed) override;
    bool size(shared_db& db, const std::string& key,
        long long& count) override;
    bool scores(shared_db& db, const std::string& key,
        const std::vector<std::string>& values,
        std::vector<zset_score>& out) override;
    bool range(shared_db& db, const std::string& key, long long start,
        long long end, bool reverse, std::vector<zset_member>& out) override;
    bool select(shared_db& db, const std::string& key, const zset_query& query,
        std::vector<zset_member>& out) override;
    bool count(shared_db& db, const std::string& key, const zset_query& query,
        long long& out) override;
    bool remove_range(shared_db& db, const std::string& key,
        const zset_query& query, long long& removed) override;
    bool store_range(shared_db& db, const std::string& source,
        const std::string& destination, const zset_query& query,
        long long& stored) override;
    bool rank(shared_db& db, const std::string& key, const std::string& member,
        bool reverse, zset_rank& out) override;
    bool pop(shared_db& db, const std::string& key, long long count,
        bool maximum, std::vector<zset_member>& out) override;
    bool mpop(shared_db& db, const std::vector<std::string>& keys,
        bool maximum, long long count, std::string& selected_key,
        zset_pop_result& out) override;
    using key_predicate = std::function<bool(const std::string&)>;
    using before_probe = std::function<bool()>;
    bool mpop_checked(shared_db& db, const std::vector<std::string>& keys,
        bool maximum, long long count, std::string& selected_key,
        zset_pop_result& out, const key_predicate& eligible,
        const before_probe& before);
    bool sample(shared_db& db, const std::string& key, long long count,
        zset_sample_result& out) override;
    bool algebra(shared_db& db, const std::vector<std::string>& keys,
        zset_operation operation, const std::string* destination,
        bool cardinality_only, const zset_options_parser& configure,
        std::vector<zset_member>& out, long long& cardinality) override;
    using geo_parser =
        std::function<bool(zset_store&, geo::search_options&)>;
    bool geo_search(shared_db& db, const std::string& source,
        const std::string* destination, const geo_parser& configure,
        geo::search_options& options, std::vector<geo::match>& out,
        long long& stored);
private:
    bool geo_collect(zset_store& store, const geo::search_options& options,
        std::vector<geo::match>& out);
    using operation = std::function<bool(zset_store&)>;
    bool run(shared_db& db, const std::string& key, const operation& callback);
    bool open(shared_db& db, page_transaction& tx, const std::string& key,
        zset_store& store, long long now);
    bool import(zset_store& store);
    bool finish(zset_store& store);
    bool result(db::batch_status status);
    bool change(zset_store& store, const zset_member& value,
        const zadd_options& options, zadd_result& out);
    bool erase(zset_store& store, const std::vector<zset_member>& values);
    bool choose(zset_store& store, long long count,
        std::vector<zset_member>& out);
    struct algebra_source;
    using source_map =
        std::map<std::string, std::unique_ptr<algebra_source>>;
    bool sources(shared_db& db, page_transaction& tx,
        const std::vector<std::string>& keys, source_map& out);
    bool finish(source_map& sources);
    bool source_error(const source_map& sources);
};

} // namespace dao
} // namespace pkv
