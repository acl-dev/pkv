//
// Created by zsx on 2023/8/13.
//

#include "stdafx.h"
#include <ctime>
#include <cstdlib>
#include <acl-lib/fiber/go_fiber.hpp>

#include "test_db.h"

namespace pkv {

test_db::test_db() = default;

test_db::~test_db() = default;

void test_db::bench(const std::string &path, size_t max) {
    srand(time(nullptr));

    go[&path, max] {
        rdb_bench(path, max);
        wdb_bench(path, max);
    };

    acl::fiber::schedule();
}

void test_db::rdb_bench(const std::string &path, size_t max) {
    auto db = db::create_rdb();
    if (!db->open(path.c_str())) {
        printf("%s: open %s db error %s\r\n",
               __func__, path.c_str(), acl::last_serror());
    } else {
        bench(db, max);
        // Close the db
        db = nullptr;
    }
}
void test_db::wdb_bench(const std::string &path, size_t max) {
    auto db = db::create_wdb();
    if (!db->open(path.c_str())) {
        printf("%s: open %s db error %s\r\n",
               __func__, path.c_str(), acl::last_serror());
    } else {
        bench(db, max);
        // Close the db
        db = nullptr;
    }
}

void test_db::bench(shared_db &db, size_t max) {
    printf("Begin test %s, count=%zd\r\n", db->get_dbtype(), max);

<<<<<<< HEAD
    struct timeval begin{};
=======
    struct timeval begin;
>>>>>>> master
    gettimeofday(&begin, nullptr);

    size_t n = bench_set(db, max);

<<<<<<< HEAD
    struct timeval end{};
=======
    struct timeval end;
>>>>>>> master
    gettimeofday(&end, nullptr);
    double cost = acl::stamp_sub(end, begin);
    double speed = ((double) n * 1000) / (cost > 0 ? cost : 0.0001);
    printf("%s: count=%zd, cost=%.2f ms, set speed=%.2f\r\n",
           db->get_dbtype(), n, cost, speed);

    gettimeofday(&begin, nullptr);
    n = bench_get(db, max);
    gettimeofday(&end, nullptr);

    cost = acl::stamp_sub(end, begin);
    speed = ((double) n * 1000) / (cost > 0 ? cost : 0.0001);
    printf("%s: count=%zd, cost=%.2f ms, get speed=%.2f\r\n",
           db->get_dbtype(), n, cost, speed);
}

size_t test_db::bench_set(pkv::shared_db &db, size_t max) {
    size_t i;
    for (i = 0; i < max; i++) {
        std::string key("key-");
        auto n = rand() % max + 1;
        key += std::to_string(n);

        std::string val("val-");
        val += std::to_string(i);
        if (!db->set(key, val)) {
            printf("%s: dbtype=%s, set error, key=%s, val=%s\r\n",
                   __func__, db->get_dbtype(), key.c_str(), val.c_str());
            break;
        }
    }
    return i;
}

size_t test_db::bench_get(pkv::shared_db &db, size_t max) {
    size_t i;
    for (i = 0; i < max; i++) {
        std::string key("key-");
        key += std::to_string(i);

        std::string val;
        if (!db->get(key, val)) {
            printf("%s: dbtype=%s, set error, key=%s\r\n",
                   __func__, db->get_dbtype(), key.c_str());
            break;
        }
    }
    return i;
}

} // namespace pkv
