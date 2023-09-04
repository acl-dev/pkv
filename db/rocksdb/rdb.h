#pragma once

#include "../db.h"

#ifdef HAS_ROCKSDB

namespace rocksdb {
    class DB;
}

namespace pkv {

class rdb_cursor;

class rdb : public db {
public:
    rdb() = default;
    ~rdb() override;

protected:
    // @override
    bool open(const char* paths) override;

    // @override
    bool set(const std::string& key, const std::string& value) override;

    // @override
    bool get(const std::string& key, std::string& value) override;

    // @override
    bool del(const std::string& key) override;

    // @override
    db_cursor* create_cursor() override;

    // @override
    bool scan(size_t idx, db_cursor& cursor,
              std::vector<std::string>& keys, size_t max) override;

    // @override
    NODISCARD size_t dbsize() const override {
        return dbs_.size();
    }

public:
    // @override
    NODISCARD const char* get_dbtype() const override {
        return "rdb";
    }

private:
    std::vector<rocksdb::DB*> dbs_;
    std::vector<std::string> paths_;

    bool open_one(const std::string& path);
};

} // namespace pkv

#endif // HAS_ROCKSDB
