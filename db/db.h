#pragma once
#include <string>

namespace pkv {

class db;
using shared_db = std::shared_ptr<db>;

class db_cursor;

class db {
public:
    db() = default;
    virtual ~db() = default;

    virtual bool open(const char* path) = 0;
    virtual bool set(const std::string& key, const std::string& value) = 0;
    virtual bool get(const std::string& key, std::string& value) = 0;
    virtual bool del(const std::string& key) = 0;
    virtual bool scan(db_cursor& cursor, std::vector<std::string>& keys, size_t max) = 0;
    virtual const char* get_dbtype() const = 0;
    virtual db_cursor* create_cursor() { return nullptr; }

public:
    static shared_db create_rdb();
    static shared_db create_wdb();
    static shared_db create_mdb();
    static shared_db create_mdb_htable();
    static shared_db create_mdb_avl();
    static shared_db create_mdb_tbb();
};

} // namespace pkv
