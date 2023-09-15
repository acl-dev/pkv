//
// Created by shuxin zheng on 2023/9/1.
//

#include "stdafx.h"
#include "db_cursor.h"

namespace pkv {

size_t db_cursor::get_db() const {
    return dbidx_;
}

void db_cursor::next_db() {
    ++dbidx_;
}

void db_cursor::reset() {
    dbidx_ = 0;
    this->clear();
}

} // namespace pkv