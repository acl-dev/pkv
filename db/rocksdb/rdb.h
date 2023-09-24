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
    bool dbopen(const char* paths) override;

    // @override
    bool dbset(const std::string& key, const std::string& value) override;

    // @override
    bool dbget(const std::string& key, std::string& value) override;

    // @override
    bool dbdel(const std::string& key) override;

    // @override
    db_cursor* dbcreate_cursor() override;

    // @override
    bool dbscan(size_t idx, db_cursor& cursor,
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
