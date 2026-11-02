#pragma once
#include "dao/list.h"
#include <functional>

namespace pkv {
namespace dao {

class json_list : public list {
public:
    virtual bool insert(shared_db& db, const std::string& key, bool before, const std::string& pivot,
            const std::string& value, long long& length) override;
    virtual bool remove(shared_db& db, const std::string& key, long long count, const std::string& value,
            long long& removed) override;
    virtual bool positions(shared_db& db, const std::string& key, const std::string& value,
            const list_search& options, std::vector<long long>& out) override;
    virtual bool move(shared_db& db, const std::string& source, const std::string& destination,
            bool from_left, bool to_left, list_result& out) override;
    virtual bool mpop(shared_db& db, const std::vector<std::string>& keys, bool left, long long count,
            std::string& selected_key, list_result& out) override;
    bool push(shared_db& db, const std::string& key, const std::vector<std::string>& values,
            bool left, bool only_existing, long long& length) override;
    bool pop(shared_db& db, const std::string& key, bool left, long long count, list_result& out) override;
    bool length(shared_db& db, const std::string& key, long long& out) override;
    bool range(shared_db& db, const std::string& key, long long start, long long end,
            std::vector<std::string>& out) override;
    bool index(shared_db& db, const std::string& key, const std::string& index, list_result& out) override;
    bool set(shared_db& db, const std::string& key, const std::string& index, const std::string& value) override;
    bool trim(shared_db& db, const std::string& key, long long start, long long end) override;
private:
    using mutator = std::function<bool(bool, value_record&, db::update_action&)>;
    bool mutate(shared_db& db, const std::string& key, const mutator& callback);
    bool batch_result(db::batch_status status);
    bool decode_list(shared_db& db, const std::string& bytes, long long now, value_record& out);
    bool read_list(shared_db& db, const std::string& key);
};

} // namespace dao
} // namespace pkv
