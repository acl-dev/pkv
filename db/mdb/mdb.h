//
// Created by shuxin ����zheng on 2023/8/21.
//

#pragma once

#include "../db.h"
#include "common/kstring.h"
#include "common/spinlock.h"

namespace pkv {

//using lock_t = acl::thread_mutex;
using lock_t = spinlock;
using map_t  = std::unordered_map<kstring, kstring>;
//using map_t  = std::map<kstring, kstring>;

class mdb : public db {
public:
    mdb();
    ~mdb() override;

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
    db_cursor* create_cursor() override {
        return nullptr;
    }

    // @override
    bool scan(size_t, db_cursor&, std::vector<std::string>&, size_t) override;

protected:
    // @override
    NODISCARD size_t dbsize() const override {
        return stores_.size();
    }

    // @override
    NODISCARD const char* get_dbtype() const override {
        return "mdb";
    }

private:
    std::vector<lock_t*> locks_;
    std::vector<map_t*> stores_;
};

} // namespace pkv
