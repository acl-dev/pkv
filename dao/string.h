#pragma once
#include "db/db.h"
#include "dao_base.h"

namespace pkv::dao {

class string : public dao_base {
public:
    string() = default;
    virtual ~string() override = default;

    virtual bool set(shared_db& db, const std::string& key, const char* data) = 0;
    virtual bool get(shared_db& db, const std::string& key, std::string& out) = 0;
};

} // namespace pkv::dao
