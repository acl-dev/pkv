//
// Created by shuxin ¡¡¡¡zheng on 2023/9/1.
//

#pragma once
#include "db/db_cursor.h"

namespace pkv {

class rdb_cursor : public db_cursor {
public:
    rdb_cursor() = default;
    ~rdb_cursor() override = default;

    void set_seek_key(const std::string& key);
    NODISCARD const std::string& get_seek_key() const {
        return seek_key_;
    }

    // @override
    void clear() override;

private:
    std::string seek_key_;
};

} // namespace pkv