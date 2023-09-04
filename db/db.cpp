#include "stdafx.h"

#ifdef HAS_ROCKSDB
#include "rocksdb/rdb.h"
#endif

#ifdef HAS_WT
#include "wt/wdb.h"
#endif

#include "mdb/mdb.h"
#include "mdb/mdb_htable.h"
#include "mdb/mdb_avl.h"
#include "mdb/mdb_tbb.h"

#include "db_cursor.h"
#include "db.h"

namespace pkv {

class dummy_db : public db {
public:
    dummy_db() = default;
    ~dummy_db() override = default;

    bool open(const char*) override {
        return false;
    }

    bool set(const std::string&, const std::string&) override {
        return false;
    }

    bool get(const std::string&, std::string&) override {
        return false;
    }

    bool del(const std::string&) override {
        return false;
    }

    db_cursor* create_cursor() override {
        return nullptr;
    }

    bool scan(size_t, db_cursor&, std::vector<std::string>&, size_t) override {
        return false;
    }

    NODISCARD const char* get_dbtype() const override {
        return "dummy_db";
    }
};

shared_db db::create_rdb() {
#ifdef HAS_ROCKSDB
    return std::make_shared<rdb>();
#else
    return std::make_shared<dummy_db>();
#endif
}

shared_db db::create_wdb() {
#ifdef HAS_WT
    return std::make_shared<wdb>();
#else
    return std::make_shared<dummy_db>();
#endif
}

shared_db db::create_mdb() {
    return std::make_shared<mdb>();
}

shared_db db::create_mdb_htable() {
    return std::make_shared<mdb_htable>();
}

shared_db db::create_mdb_avl() {
    return std::make_shared<mdb_avl>();
}

shared_db db::create_mdb_tbb() {
#ifdef HAS_TBB
    return std::make_shared<mdb_tbb>();
#else
    return std::make_shared<dummy_db>();
#endif
}

bool db::scan(db_cursor& cursor, std::vector<std::string>& keys, size_t max) {
    keys.clear();

    while (true) {
        size_t idx = cursor.get_db();
        if (idx >= this->dbsize()) {
            return true;
        }

        if (!scan(idx, cursor, keys, max)) {
            return false;
        }

        if (keys.size() >= max) {
            return true;
        }

        // If not got the needed keys, we should clear the seek key for next db.
        cursor.clear();
        cursor.next_db();
    }
}

} // namespace pkv