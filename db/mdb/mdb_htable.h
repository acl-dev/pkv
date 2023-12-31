//
// Created by shuxin zheng on 2023/8/21.
//

#pragma once

#include "../db.h"

namespace pkv {

class mdb_htable_cursor;

class mdb_htable : public db {
public:
    mdb_htable(size_t count);
    ~mdb_htable() override;

protected:
    // @override
    bool dbopen(const char* path) override;

    // @override
    bool dbset(const std::string& key, const std::string& value) override;

    // @override
    bool dbget(const std::string& key, std::string& value) override;

    // @override
    bool dbdel(const std::string& key) override;

    // @override
    db_cursor* dbcreate_cursor() override;

    // @override
    bool dbscan(size_t, db_cursor&, std::vector<std::string>&, size_t) override;

protected:
    // @override
    NODISCARD size_t dbsize() const override {
        return dbs_.size();
    }

    // @override
    NODISCARD const char* get_dbtype() const override {
        return "mdb";
    }

private:
    std::vector<ACL_HTABLE*> dbs_;
    ACL_SLICE_POOL* slice_;

};

} // namespace pkv
