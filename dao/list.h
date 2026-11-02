#pragma once
#include "dao_base.h"

namespace pkv {
namespace dao {

struct list_result {
    bool found = false;
    std::vector<std::string> values;
};

class list : public dao_base {
public:
    virtual bool insert(shared_db& db, const std::string& key, bool before, const std::string& pivot,
            const std::string& value, long long& length) = 0;
    virtual bool remove(shared_db& db, const std::string& key, long long count, const std::string& value,
            long long& removed) = 0;
    virtual bool positions(shared_db& db, const std::string& key, const std::string& value,
            const list_search& options, std::vector<long long>& out) = 0;
    virtual bool move(shared_db& db, const std::string& source, const std::string& destination,
            bool from_left, bool to_left, list_result& out) = 0;
    virtual bool mpop(shared_db& db, const std::vector<std::string>& keys, bool left, long long count,
            std::string& selected_key, list_result& out) = 0;
    virtual bool push(shared_db& db, const std::string& key, const std::vector<std::string>& values,
            bool left, bool only_existing, long long& length) = 0;
    virtual bool pop(shared_db& db, const std::string& key, bool left, long long count, list_result& out) = 0;
    virtual bool length(shared_db& db, const std::string& key, long long& out) = 0;
    virtual bool range(shared_db& db, const std::string& key, long long start, long long end,
            std::vector<std::string>& out) = 0;
    // Index parsing follows key lookup/type checking, matching Redis precedence.
    virtual bool index(shared_db& db, const std::string& key, const std::string& index, list_result& out) = 0;
    virtual bool set(shared_db& db, const std::string& key, const std::string& index, const std::string& value) = 0;
    virtual bool trim(shared_db& db, const std::string& key, long long start, long long end) = 0;
};

} // namespace dao
} // namespace pkv
