#pragma once
#include "dao/set.h"

namespace pkv {
namespace dao {

class json_set : public set {
public:
    virtual bool sample(shared_db& db, const std::string& key, long long count, bool remove,
            set_sample_result& out) override;
    virtual bool intersection_size(shared_db& db, const std::vector<std::string>& keys,
            long long limit, long long& count) override;
    bool change(shared_db& db, const std::string& key, const std::vector<std::string>& members,
            bool add, long long& changed) override;
    bool size(shared_db& db, const std::string& key, long long& count) override;
    bool membership(shared_db& db, const std::string& key, const std::vector<std::string>& members,
            std::vector<long long>& out) override;
    bool members(shared_db& db, const std::string& key, std::set<std::string>& out) override;
    bool move(shared_db& db, const std::string& source, const std::string& destination,
            const std::string& member, bool& moved) override;
    bool algebra(shared_db& db, const std::vector<std::string>& keys, set_operation operation,
            const std::string* destination, std::set<std::string>& out) override;
private:
    bool choose_sample(const std::set<std::string>& members, long long count, std::vector<std::string>& out);
    bool read_set(shared_db& db, const std::string& key);
    bool decode_set(shared_db& db, const std::string& bytes, long long now, value_record& out);
    bool batch_result(db::batch_status status);
};

} // namespace dao
} // namespace pkv
