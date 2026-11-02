#include "stdafx.h"
#include "db/db.h"
#include "db/db_watcher.h"
#include <cstdlib>
#include <iostream>
#include <unistd.h>

using namespace pkv;

static void require(bool condition, const char* message) {
    if (condition) return;
    std::cerr << message << '\n'; std::exit(1);
}

static shared_db create(const std::string& backend) {
    return backend == "rdb" ? db::create_rdb() : db::create_wdb();
}

static void run(const std::string& backend, const std::string& root) {
    auto database = create(backend);
    const auto untouched = root + "/not-opened";
    require(!database->open_paths({}) && !database->open_paths({""}),
        "empty native paths accepted");
    require(!database->open_paths({untouched,
        std::string("invalid\0suffix", 14)}) &&
        ::access(untouched.c_str(), F_OK), "NUL path created storage");
    const std::vector<std::string> paths{
        root + "/space path", root + "/semi;comma,tab\tpath"};
    auto watchers = std::make_shared<db_watchers>(7);
    std::weak_ptr<db_watchers> retained(watchers);
    require(database->open_observed_paths(paths, watchers),
        "exact native roots failed");
    watchers.reset();
    require(!retained.expired(), "observed paths lost watcher ownership");
    const std::string value = "{\"T\":\"S\",\"E\":-1,\"D\":\"paths\"}";
    for (unsigned i = 0; i < 16; ++i)
        require(database->set(std::string("key\0", 4) + std::to_string(i),
            value), "native path write failed");
    require(database->close_for_exit(), "native path close failed");
    database.reset();
    require(retained.expired(), "native path watcher owner leaked");
    database = create(backend);
    require(database->open_paths(paths), "native path reopen failed");
    for (unsigned i = 0; i < 16; ++i) {
        std::string actual;
        require(database->get(std::string("key\0", 4) + std::to_string(i),
            actual) && actual == value, "native path data changed");
    }
    require(database->close_for_exit(), "native path second close failed");
    auto memory = db::create_mdb(2);
    require(!memory->open_paths(paths) && memory->open(nullptr),
        "unsupported path API changed memory startup");
}

int main(int argc, char** argv) {
    acl::acl_cpp_init();
    require(argc == 3, "expected backend and scratch directory");
    const std::string backend(argv[1]);
    require(backend == "rdb" || backend == "wdb", "invalid backend");
    run(backend, argv[2]);
    std::cout << "Exact native paths, validation and observers passed\n";
}
