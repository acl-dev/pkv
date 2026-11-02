#include "stdafx.h"
#include "db/move_domain.h"
#include "db/move_recovery.h"
#include "db/database_pool.h"
#include "db/page_transaction.h"
#include "coder/string_page.h"
#include <iostream>

using namespace pkv;
static const std::string owner("start\0owner", 11);
static void require(bool ok, const char* reason) {
    if (!ok) { std::cerr << reason << '\n'; std::exit(1); }
}
static shared_db open(const std::string& backend,
    const std::vector<std::string>& roots, unsigned index) {
    auto database = backend == "rdb" ? db::create_rdb() : db::create_wdb();
    std::string paths;
    for (const auto& root : roots) {
        if (!paths.empty()) paths += ';';
        paths += root;
        if (index) paths += "/logical-db-" + std::to_string(index);
    }
    require(database && database->open(paths.c_str()), "seed native open");
    return database;
}
static void seed(shared_db database, const std::string& key, int64_t expiry) {
    require(database->transact_pages({key}, [&](page_transaction& tx) {
        auto& root = *tx.roots().find(key);
        string_root value; value.length = 32768; value.pages = 2;
        value.expires_at_ms = expiry;
        if (!tx.reset_pages(key) || string_page_codec::encode_root(value,
            root.value) != string_page_status::ok) return false;
        root.action = db::update_action::write;
        return tx.put(key, 1, std::string(16384, 'v')) &&
            tx.put(key, 2, std::string(16384, 'v'));
    }) == db::batch_status::ok, "seed native value");
}
int main(int argc, char** argv) {
    require(argc == 5, "backend mode directory phase");
    acl::acl_cpp_init();
    const std::string backend(argv[1]), mode(argv[2]), root(argv[3]);
    const std::string phase(argv[4]);
    move_domain domain;
    require(domain.prepare(backend, root + "/a;" + root + "/b", 16,
        root + "/decision").empty(), "seed domain prepare");
    auto from = open(backend, domain.roots(), 0);
    auto to = open(backend, domain.roots(), 1);
    require(domain.bind().empty() && domain.journal(), "seed domain bind");
    if (mode == "--check") {
        require(domain.journal()->state() == move_journal::status::empty,
            "server did not finish recovery");
        std::string bytes;
        require(!from->get(owner, bytes), "source retained after server redo");
        const bool found = to->get(owner, bytes);
        require(found != (phase == "expired"), "target after server redo");
        require(from->get("neighbor", bytes) && to->get("neighbor", bytes),
            "server recovery touched unrelated owner");
    } else {
        require(mode == "--seed", "seed mode");
        seed(from, owner, phase == "expired" ? 1 : 4102444800000LL);
        seed(from, "neighbor", -1); seed(to, "neighbor", -1);
        require(from->sync_key(owner) == db::sync_status::ok &&
            from->sync_key("neighbor") == db::sync_status::ok &&
            to->sync_key("neighbor") == db::sync_status::ok,
            "source WAL before startup decision");
        move_decision decision;
        decision.source = 0; decision.target = 1; decision.serial = 77;
        decision.owner = owner;
        require(domain.journal()->decide(decision), "seed startup decision");
        if (phase == "page" || phase == "target" || phase == "erased") {
            database_pool pool(from, 16, [to](size_t id) {
                return id == 1 ? to : shared_db();
            });
            move_recovery::run(pool, *domain.journal(),
                [&](move_recovery::step at) {
                if ((phase == "page" && at == move_recovery::step::page_copied)
                    || (phase == "target" && at ==
                        move_recovery::step::target_published)
                    || (phase == "erased" && at ==
                        move_recovery::step::source_erased)) std::_Exit(0);
            });
            require(false, "startup seed cutpoint not reached");
        }
    }
    std::cout << "Service startup native fixture passed\n";
}
