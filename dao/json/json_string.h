#pragma once
#include "db/db.h"
#include "../string.h"

namespace pkv {
namespace dao {

class json_string : public string {
public:
    json_string();
    ~json_string() override = default;

    // @override
    bool set(shared_db& db, const std::string& key, const char* data) override;

    // @override
    bool get(shared_db& db, const std::string& key, std::string& out) override;

public:
    void set_string(const char* data);
    bool to_string(std::string& out);

protected:
    const char* build();

private:
    const char* data_;
};

} // namespace dao
} // namespace pkv
