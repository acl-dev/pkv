#pragma once
#include "dao_base.h"

namespace pkv {
namespace dao {

enum class set_operation { unite, intersect, difference };

struct set_sample_result {
    bool found = false;
    std::vector<std::string> values;
};

class set : public dao_base {
public:
    virtual bool sample(shared_db& db, const std::string& key, long long count, bool remove,
            set_sample_result& out) = 0;
    virtual bool intersection_size(shared_db& db, const std::vector<std::string>& keys,
            long long limit, long long& count) = 0;
    virtual bool change(shared_db& db, const std::string& key, const std::vector<std::string>& members,
            bool add, long long& changed) = 0;
    virtual bool size(shared_db& db, const std::string& key, long long& count) = 0;
    virtual bool membership(shared_db& db, const std::string& key, const std::vector<std::string>& members,
            std::vector<long long>& out) = 0;
    virtual bool members(shared_db& db, const std::string& key, std::set<std::string>& out) = 0;
    virtual bool move(shared_db& db, const std::string& source, const std::string& destination,
            const std::string& member, bool& moved) = 0;
    // A null destination returns a read-only snapshot. Otherwise replace the
    // destination with a persistent result, deleting it when the result is empty.
    virtual bool algebra(shared_db& db, const std::vector<std::string>& keys, set_operation operation,
            const std::string* destination, std::set<std::string>& out) = 0;
};

} // namespace dao
} // namespace pkv
