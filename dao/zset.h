#pragma once
#include "dao_base.h"

namespace pkv {
namespace dao {
struct zadd_options { bool nx = false, xx = false, gt = false, lt = false, incr = false, ch = false; };
struct zadd_result { long long added = 0, changed = 0; bool processed = false; double score = 0; };
struct zset_sample_result { bool found = false; std::vector<zset_member> values; };
struct zset_pop_result { bool found = false; std::vector<zset_member> values; };
enum class zset_operation { unite, intersect, difference };
enum class zset_aggregate { sum, minimum, maximum };
struct zset_algebra_options {
    std::vector<double> weights;
    zset_aggregate aggregate = zset_aggregate::sum;
    long long limit = 0;
};
// Invoked after source validation, under transaction locks. Only parse options;
// do not perform I/O, retain borrowed state or recurse into this DB.
using zset_options_parser = std::function<bool(zset_algebra_options&)>;
class zset : public dao_base {
public:
    virtual bool algebra(shared_db& db, const std::vector<std::string>& keys, zset_operation operation,
            const std::string* destination, bool cardinality_only, const zset_options_parser& configure,
            std::vector<zset_member>& out, long long& cardinality) = 0;
    virtual bool mpop(shared_db& db, const std::vector<std::string>& keys, bool maximum, long long count,
            std::string& selected_key, zset_pop_result& out) = 0;
    virtual bool sample(shared_db& db, const std::string& key, long long count, zset_sample_result& out) = 0;
    virtual bool select(shared_db& db, const std::string& key, const zset_query& query, std::vector<zset_member>& out) = 0;
    virtual bool count(shared_db& db, const std::string& key, const zset_query& query, long long& out) = 0;
    virtual bool remove_range(shared_db& db, const std::string& key, const zset_query& query, long long& removed) = 0;
    virtual bool store_range(shared_db& db, const std::string& source, const std::string& destination,
            const zset_query& query, long long& stored) = 0;
    virtual bool add(shared_db& db, const std::string& key, const std::vector<zset_member>& values,
            const zadd_options& options, zadd_result& out) = 0;
    virtual bool remove(shared_db& db, const std::string& key, const std::vector<std::string>& values, long long& removed) = 0;
    virtual bool size(shared_db& db, const std::string& key, long long& count) = 0;
    virtual bool scores(shared_db& db, const std::string& key, const std::vector<std::string>& values, std::vector<zset_score>& out) = 0;
    virtual bool range(shared_db& db, const std::string& key, long long start, long long end, bool reverse, std::vector<zset_member>& out) = 0;
    virtual bool rank(shared_db& db, const std::string& key, const std::string& member, bool reverse, zset_rank& out) = 0;
    virtual bool pop(shared_db& db, const std::string& key, long long count, bool maximum, std::vector<zset_member>& out) = 0;
};
}
}
