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
#ifdef HAS_TBB
#include "mdb/mdb_tbb.h"
#endif

#include "db_cursor.h"
#include "db.h"

namespace pkv {

class dummy_db : public db {
public:
    dummy_db() = default;
    ~dummy_db() override = default;

    bool dbopen(const char*) override {
        return false;
    }

    bool dbset(const std::string&, const std::string&) override {
        return false;
    }

    bool dbget(const std::string&, std::string&) override {
        return false;
    }

    bool dbdel(const std::string&) override {
        return false;
    }

    db_cursor* dbcreate_cursor() override {
        return nullptr;
    }

    bool dbscan(size_t, db_cursor&, std::vector<std::string>&, size_t) override {
        return false;
    }

    NODISCARD const char* get_dbtype() const override {
        return "dummy_db";
    }

    size_t dbsize() const override {
        return 0;
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

bool db::open(const char *path) {
    return this->dbopen(path);
}

bool db::set(const std::string &key, const std::string &value) {
    return this->dbset(key, value);
}

bool db::get(const std::string &key, std::string &value) {
    return this->dbget(key, value);
}

bool db::del(const std::string &key) {
    return this->dbdel(key);
}

db_cursor *db::create_cursor() {
    return this->dbcreate_cursor();
}

bool db::scan(db_cursor& cursor, std::vector<std::string>& keys, size_t max) {
    keys.clear();

    while (true) {
        size_t idx = cursor.get_db();
        if (idx >= this->dbsize()) {
            return true;
        }

        if (!this->dbscan(idx, cursor, keys, max)) {
            return false;
        }

        if (keys.size() >= max) {
            return true;
        }

        // If the keys are not enough, we should use the next db to get more,
        // and clear the current cursor.
        cursor.clear();
        cursor.next_db();
    }
}

} // namespace pkv
