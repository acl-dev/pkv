#pragma once
#include <yyjson.h>
#include "db/db.h"

namespace pkv::dao {

class dao_base {
public:
    dao_base();
    virtual ~dao_base();

public:
    static bool save(shared_db& db, const std::string& key, const std::string& data);
    yyjson_val* read(shared_db& db, const std::string& key);

public:
    void reset();

protected:
    bool finished_;
    std::string type_;
    time_t expire_;
    char* result_;
    yyjson_mut_doc* w_doc_;
    yyjson_mut_val* w_root_;
    yyjson_doc* r_doc_;
    yyjson_val* r_root_;

    void create_writer();
};

} // namespace pkv::dao