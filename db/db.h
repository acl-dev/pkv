#pragma once

namespace pkv {

class db;
using shared_db = std::shared_ptr<db>;

class db {
public:
    db() = default;
    virtual ~db() = default;

    virtual bool open(const char* path) = 0;
    virtual bool set(const std::string& key, const std::string& value) = 0;
    virtual bool get(const std::string& key, std::string& value) = 0;
    virtual bool del(const std::string& key) = 0;
    virtual bool scan(const std::string& seek_key,
	std::vector<std::string>& keys, size_t max) = 0;

public:
    virtual const char* get_dbtype() const = 0;

    static shared_db create_rdb();
    static shared_db create_wdb();
};

} // namespace pkv
