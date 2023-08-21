//
// Created by shuxin ����zheng on 2023/8/21.
//

#pragma once
#include "../db.h"

#define USE_HTABLE

namespace pkv {

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
    bool scan(const std::string& key, std::vector<std::string>& keys,
          size_t max) override;

public:
    // @override
    NODISCARD const char* get_dbtype() const override {
        return "mdb";
    }

private:
#ifdef USE_HTABLE
    ACL_HTABLE* store_;
    ACL_SLICE_POOL* slice_;
#else
    std::unordered_map<std::string, std::string> store_;
#endif
};

} // namespace pkv
