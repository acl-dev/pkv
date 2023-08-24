//
// Created by shuxin ¡¡¡¡zheng on 2023/8/21.
//

#pragma once

#if defined(HAS_TBB)
# include <tbb/concurrent_hash_map.h>

#include "../db.h"

namespace pkv {

class mdb_tbb : public db {
public:
    mdb_tbb();
    ~mdb_tbb() override;

protected:
    // @override
    bool open(const char* path) override;

    // @override
    bool set(const std::string& key, const std::string& value) override;

    // @override
    bool get(const std::string& key, std::string& value) override;

    // @override
    bool del(const std::string& key) override;

    // @override
    bool scan(const std::string& key, std::vector<std::string>& keys,
          size_t max) override;

    // @override
    NODISCARD const char* get_dbtype() const override {
        return "mdb";
    }

private:
    tbb::concurrent_hash_map<std::string, std::string> store_;
};

} // namespace pkv

#endif // HAS_TBB
