#pragma once
#include "db/db.h"
#include "dao_base.h"

namespace pkv::dao {

class key : public dao_base {
public:
    key() = default;
    virtual ~key() override = default;

    virtual bool del(shared_db& db, const std::string& key) = 0;
    virtual bool type(shared_db& db, const std::string& key, std::string& out) = 0;
    virtual bool expire(shared_db& db, const std::string& key, int n) = 0;
    virtual bool ttl(shared_db& db, const std::string& key, int& n) = 0;
    virtual bool scan(shared_db& db, const std::string& seek_key,
	      std::vector<std::string>& keys, size_t max) = 0;

private:
};

} // namespace pkv::dao
