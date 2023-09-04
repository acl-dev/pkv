//
// Created by shuxin ����zheng on 2023/8/21.
//

#pragma once

#include "../db.h"

namespace pkv {

class mdb_htable_cursor;

class mdb_htable : public db {
public:
    mdb_htable();
    ~mdb_htable() override;

protected:
    // @override
    bool open(const char* path) override;

    // @override
    bool set(const std::string& key, const std::string& value) override;

    // @override
    bool get(const std::string& key, std::string& value) override;

    // @override
    bool del(const std::string& key) override;

    // @override
    db_cursor* create_cursor() override;

    // @override
    bool scan(size_t, db_cursor&, std::vector<std::string>&, size_t) override;

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
