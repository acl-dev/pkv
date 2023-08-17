#pragma once
#include "db/db.h"
#include "dao_base.h"

namespace pkv::dao {

class key : public dao_base {
public:
    key() = default;
    ~key() override = default;

    bool del(shared_db& db, const std::string& key);
    bool type(shared_db& db, const std::string& key, std::string& out);

private:
};

} // namespace pkv::dao