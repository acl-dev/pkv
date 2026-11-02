#pragma once
#include "dao/zset.h"
#include <functional>

namespace pkv {
namespace dao {
class json_zset : public zset {
public:
    bool algebra(shared_db& db, const std::vector<std::string>& keys, zset_operation operation,
            const std::string* destination, bool cardinality_only, const zset_options_parser& configure,
            std::vector<zset_member>& out, long long& cardinality) override;
    bool mpop(shared_db& db, const std::vector<std::string>& keys, bool maximum, long long count,
            std::string& selected_key, zset_pop_result& out) override;
    bool sample(shared_db& db, const std::string& key, long long count, zset_sample_result& out) override;
    virtual bool select(shared_db& db, const std::string& key, const zset_query& query, std::vector<zset_member>& out) override;
    virtual bool count(shared_db& db, const std::string& key, const zset_query& query, long long& out) override;
    virtual bool remove_range(shared_db& db, const std::string& key, const zset_query& query, long long& removed) override;
    virtual bool store_range(shared_db& db, const std::string& source, const std::string& destination,
            const zset_query& query, long long& stored) override;
    bool add(shared_db& db, const std::string& key, const std::vector<zset_member>& values,
            const zadd_options& options, zadd_result& out) override;
    bool remove(shared_db& db, const std::string& key, const std::vector<std::string>& values, long long& removed) override;
    bool size(shared_db& db, const std::string& key, long long& count) override;
    bool scores(shared_db& db, const std::string& key, const std::vector<std::string>& values, std::vector<zset_score>& out) override;
    bool range(shared_db& db, const std::string& key, long long start, long long end, bool reverse, std::vector<zset_member>& out) override;
    bool rank(shared_db& db, const std::string& key, const std::string& member, bool reverse, zset_rank& out) override;
    bool pop(shared_db& db, const std::string& key, long long count, bool maximum, std::vector<zset_member>& out) override;
private:
    using mutator = std::function<bool(bool, value_record&, db::update_action&)>;
    bool mutate(shared_db& db, const std::string& key, const mutator& callback);
    bool read_zset(shared_db& db, const std::string& key);
};
}
}
