//
// Created by zsx on 2023/9/3.
//

#pragma once
#include "db/db_cursor.h"

namespace pkv {

class mdb_htable;

class mdb_htable_cursor : public db_cursor {
public:
    mdb_htable_cursor(mdb_htable& htable);
    ~mdb_htable_cursor() = default;

private:
    friend class mdb_htable;
    size_t idx_ = 0;

    mdb_htable& htable_;
    ACL_HTABLE_ITER iter_;
};

} // namespace pkv