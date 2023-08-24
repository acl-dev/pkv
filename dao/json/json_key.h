#pragma once
#include "db/db.h"
#include "../key.h"

namespace pkv {
namespace dao {

class json_key : public key {
public:
    json_key() = default;
    ~json_key() override = default;

    // @override
    bool del(shared_db& db, const std::string& json_key) override;

    // @override
    bool type(shared_db& db, const std::string& json_key, std::string& out) override;

    // @override
    bool expire(shared_db& db, const std::string& json_key, int n) override;

    // @override
    bool ttl(shared_db& db, const std::string& json_key, int& n) override;

    // @override
    bool scan(shared_db& db, const std::string& seek_json_key,
	      std::vector<std::string>& json_keys, size_t max) override;
};

} // namespace dao
} // namespace pkv
