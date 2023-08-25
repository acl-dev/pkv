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

    bool scan(const std::string&, std::vector<std::string>&, size_t) override {
        return false;
    }

    const char* get_dbtype() const override {
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

} // namespace pkv
