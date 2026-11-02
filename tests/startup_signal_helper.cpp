#include "stdafx.h"
#include "common/shutdown_signals.h"
#include "db/database_pool.h"
#include "db/move_domain.h"
#include "db/move_recovery.h"
#include <csignal>
#include <stdexcept>

using namespace pkv;
static void require(bool ok) { if (!ok) std::_Exit(2); }
static void pause_startup() {
    std::puts("READY"); std::fflush(stdout);
    char byte;
    (void) ::read(STDIN_FILENO, &byte, 1);
    std::_Exit(2);
}
static shared_db open_database(const std::string& backend,
    const std::string& paths) {
    auto value = backend == "rdb" ? db::create_rdb() : db::create_wdb();
    require(value && value->open(paths.c_str()));
    return value;
}
static void recovering(const std::string& backend, const std::string& root,
    const std::string& phase) {
    move_domain domain;
    const auto paths = root + "/a;" + root + "/b";
    require(domain.prepare(backend, paths, 16, root + "/decision").empty());
    auto from = open_database(backend, paths);
    auto to = open_database(backend,
        root + "/a/logical-db-1;" + root + "/b/logical-db-1");
    require(domain.bind().empty() && domain.journal());
    database_pool pool(from, 16, [to](size_t index) {
        return index == 1 ? to : shared_db();
    });
    move_recovery::run(pool, *domain.journal(),
        [&](move_recovery::step at) {
        if ((phase == "page" && at == move_recovery::step::page_copied) ||
            (phase == "target" && at ==
                move_recovery::step::target_published) ||
            (phase == "erased" && at == move_recovery::step::source_erased))
            pause_startup();
    });
    require(false);
}
int main(int argc, char** argv) {
    require(argc == 5);
    require(block_shutdown_signals());
    require(!enable_shutdown_signals([](int) { std::_Exit(2); }));
    require(start_shutdown_signals() && !start_shutdown_signals());
    acl::acl_cpp_init();
    const std::string backend(argv[1]), mode(argv[2]), root(argv[3]);
    if (mode == "recover") recovering(backend, root, argv[4]);
    shared_db database;
    if (mode == "native") database = open_database(backend, root);
    if (mode == "ready" || mode == "throw" || mode == "return") {
        require(enable_shutdown_signals([mode](int received) {
            require(received == SIGTERM || received == SIGINT);
            if (mode == "throw") throw std::runtime_error("stop failure");
            if (mode != "return") std::_Exit(7);
        }));
        require(!enable_shutdown_signals([](int) { std::_Exit(2); }));
    }
    pause_startup();
}
