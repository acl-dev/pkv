//
// Created by shuxin zheng on 2023/8/21.
//

#pragma once

#if defined(HAS_TBB)
# include <tbb/concurrent_hash_map.h>

#include "../db.h"
#include "common/kstring.h"

using string_t = pkv::kstring;
//using string_t = std::string;

namespace pkv {

using atomic_map_t = tbb::concurrent_hash_map<string_t, string_t>;

class mdb_tbb : public db {
public:
    mdb_tbb();
    ~mdb_tbb() override;

protected:
    // @override
    bool dbopen(const char* path) override;

    // @override
    bool dbset(const std::string& key, const std::string& value) override;

    // @override
    bool dbget(const std::string& key, std::string& value) override;

    // @override
    bool dbdel(const std::string& key) override;

    // @override
    db_cursor* dbcreate_cursor() override {
        return nullptr;
    }

    // @override
    bool dbscan(size_t, db_cursor&, std::vector<std::string>&, size_t) override;

protected:
    // @override
    NODISCARD size_t dbsize() const override {
        return stores_.size();
    }

    // @override
    NODISCARD const char* get_dbtype() const override {
        return "mdb";
    }

private:
    std::vector<atomic_map_t*> stores_;
};

} // namespace pkv

#endif // HAS_TBB
