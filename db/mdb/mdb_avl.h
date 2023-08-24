//
// Created by shuxin ¡¡¡¡zheng on 2023/8/24.
//

#pragma once
#include "../db.h"

namespace pkv {

class mdb_avl : public db {
public:
    mdb_avl();
    ~mdb_avl() override;

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

    // @override
    NODISCARD const char* get_dbtype() const override {
        return "mdb_aval";
    }

private:
    std::string table_name_;
    ACL_MDB* mdb_;
    ACL_MDT* mdt_;
};

} // namespace pkv