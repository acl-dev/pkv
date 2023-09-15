//
// Created by shuxin zheng on 2023/7/27.
//

#pragma once

#include "../db.h"

#ifdef HAS_WT

namespace pkv {

class wt;

class wdb : public db {
public:
    explicit wdb(size_t cache_max = 10000);
    ~wdb() override;

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
    db_cursor* create_cursor() override {
        return nullptr;
    }

    // @override
    bool scan(size_t, db_cursor&, std::vector<std::string>&, size_t) override;

protected:
    // @override
    NODISCARD size_t dbsize() const override {
        return dbs_.size();
    }

    // @override
    NODISCARD const char* get_dbtype() const override {
        return "wdb";
    }

private:
    size_t cache_max_;
    std::vector<wt*> dbs_;

    bool open_one(const std::string& path);
};

} // namespace pkv

#endif // HAS_WT
