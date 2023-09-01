//
// Created by shuxin ¡¡¡¡zheng on 2023/9/1.
//

#include "stdafx.h"
#include "db_cursor.h"

namespace pkv {

void db_cursor::set_seek_key(const std::string &key) {
    seek_key_ = key;
}

size_t db_cursor::get_db() const {
    return dbidx_;
}

void db_cursor::next_db() {
    ++dbidx_;
}

void db_cursor::reset() {
    seek_key_.clear();
    dbidx_ = 0;
}

} // namespace pkv