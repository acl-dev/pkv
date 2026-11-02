#include "stdafx.h"
#include "db/database_pool.h"
#include "db/mdb/mdb.h"
#include "common/execution_gate.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>

using namespace pkv;
static void require(bool ok, const char* text) {
    if (!ok) { std::cerr << text << '\n'; std::exit(1); }
}
class exit_database : public mdb {
public:
    exit_database() : mdb(1) {}
    unsigned calls = 0;
    bool fail = false, throwing = false;
    std::atomic<bool>* active = nullptr;
protected:
    bool dbclose_for_exit() override {
        require(!active || !active->load(), "close overlapped a command");
        ++calls;
        if (throwing) throw std::bad_alloc();
        return !fail;
    }
};
static std::shared_ptr<exit_database> opened() {
    auto value = std::make_shared<exit_database>();
    require(value->open(""), "shutdown DB open"); return value;
}
static void outcomes() {
    for (int failing = -1; failing < 3; ++failing)
        for (bool throwing : {false, true}) {
            std::vector<std::shared_ptr<exit_database>> values{
                opened(), opened(), opened()};
            unsigned factories = 0;
            database_pool pool(values[0], 4, [&](size_t id) -> shared_db {
                ++factories;
                return id < values.size() ? values[id] : opened();
            });
            require(pool.get(1) && pool.get(2), "shutdown materialize");
            if (failing >= 0) {
                values[failing]->fail = true;
                values[failing]->throwing = throwing;
            }
            execution_guard admission(true);
            require(pool.close_for_exit() == (failing < 0),
                "storage shutdown failure was hidden");
            for (const auto& value : values)
                require(value->calls == 1, "shutdown skipped another DB");
            require(!pool.get(3) && factories == 2,
                "terminal shutdown reopened a DB");
            if (failing < 0) {
                require(pool.close_for_exit(), "repeat successful close");
                for (const auto& value : values)
                    require(value->calls == 1, "closed twice");
            }
        }
}
static void draining() {
    auto value = opened();
    std::atomic<bool> active{false}; value->active = &active;
    database_pool pool(value, 1, {});
    std::promise<void> entered, release;
    auto resumed = release.get_future();
    auto reader = std::async(std::launch::async, [&] {
        execution_guard command;
        active.store(true); entered.set_value();
        resumed.get();
        require(value->set("last", "acknowledged"), "last command write");
        active.store(false);
    });
    entered.get_future().get();
    auto closer = std::async(std::launch::async, [&] {
        execution_guard admission(true);
        return pool.close_for_exit();
    });
    require(closer.wait_for(std::chrono::milliseconds(20)) ==
        std::future_status::timeout, "shutdown bypassed command admission");
    release.set_value(); reader.get();
    require(closer.get() && value->calls == 1, "drained close failed");
}
int main() {
    acl::acl_cpp_init(); outcomes(); draining();
    std::cout << "Shutdown drains commands, closes all DBs "
        "and reports errors\n";
}
