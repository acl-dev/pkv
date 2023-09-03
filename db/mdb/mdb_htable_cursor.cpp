//
// Created by zsx on 2023/9/3.
//

#include "stdafx.h"
#include "mdb_htable.h"
#include "mdb_htable_cursor.h"

namespace pkv {

mdb_htable_cursor::mdb_htable_cursor(mdb_htable &htable) : htable_(htable) {}

} // namespace pkv