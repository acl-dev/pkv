//
// Created by shuxin zheng on 2023/9/1.
//

#include "stdafx.h"
#include "rdb_cursor.h"

namespace pkv {

void rdb_cursor::set_seek_key(const std::string &key) {
    seek_key_ = key;
    started_ = true;
}

void rdb_cursor::clear() {
    seek_key_.clear();
    started_ = false;
}

} // namespace pkv
