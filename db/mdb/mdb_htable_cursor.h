//
// Created by zsx on 2023/9/3.
//

#pragma once
#include "db/db_cursor.h"

namespace pkv {

class mdb_htable;

class mdb_htable_cursor : public db_cursor {
public:
    explicit mdb_htable_cursor() = default;
    ~mdb_htable_cursor() override = default;

    // @override
    void clear() override;

private:
    friend class mdb_htable;
    size_t idx_ = 0;

<<<<<<< HEAD
    ACL_HTABLE_ITER iter_{};
};

} // namespace pkv
=======
    ACL_HTABLE_ITER iter_;
};

} // namespace pkv
>>>>>>> master
