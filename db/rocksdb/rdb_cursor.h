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
};

} // namespace pkv