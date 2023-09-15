//
// Created by zsx on 2023/9/3.
//

#include "stdafx.h"
#include "mdb_htable.h"
#include "mdb_htable_cursor.h"

namespace pkv {

void mdb_htable_cursor::clear() {
    idx_ = 0;
}

} // namespace pkv