#include "stdafx.h"
#include "db/db.h"
#include "db/db_watcher.h"
#include "db/mdb/mdb.h"
#include "db/page_transaction.h"
#include "coder/list_page.h"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <new>
#include <set>
#include <stdexcept>
#include <thread>

// Fault injection covers staging, native preparation and native insertion.
static thread_local long allocation_budget = -1;
static thread_local bool measure_allocations = false;
static thread_local size_t allocated_bytes = 0;
void* operator new(std::size_t size) {
    if (allocation_budget == 0) {
        throw std::bad_alloc();
    }
    if (allocation_budget > 0) {
        --allocation_budget;
    }
    if (measure_allocations) {
        allocated_bytes += size;
    }
    if (void* pointer = std::malloc(size ? size : 1)) {
        return pointer;
    }
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
#if __cplusplus >= 201402L
void operator delete(void* pointer, std::size_t) noexcept {
    std::free(pointer);
}
void operator delete[](void* pointer, std::size_t) noexcept {
    std::free(pointer);
}
#endif

using namespace pkv;
using batch_status = db::batch_status;
using lookup_status = db::lookup_status;
using action = db::update_action;

static void require(bool ok, const char* reason) {
    if (!ok) {
        std::cerr << reason << '\n';
        std::exit(1);
    }
}

static shared_db memory() {
    auto database = db::create_mdb(8);
    require(database->open(""), "memory open");
    return database;
}

static void put_root(page_transaction& transaction, const std::string& key,
    const std::string& value) {
    auto& root = *transaction.roots().find(key);
    root.value = value;
    root.action = action::write;
}

static std::string read_page(shared_db database, const std::string& owner,
    uint64_t id, bool found = true) {
    std::string out = "sentinel";
    require(database->transact_pages({owner}, [&](page_transaction& view) {
        const auto status = view.lookup(owner, id, out);
        require(status == (found ? lookup_status::found :
            lookup_status::missing), "page lookup status");
        return true;
    }) == batch_status::ok, "page read transaction");
    require(found || out == "sentinel", "missing page changed output");
    return out;
}

static void seed(shared_db database, const std::string& key, size_t count) {
    require(database->transact_pages({key}, [&](page_transaction& view) {
        put_root(view, key, "root");
        for (size_t i = 1; i <= count; ++i) {
            require(view.put(key, i, "page:" + std::to_string(i)),
                "seed page stage");
        }
        return true;
    }) == batch_status::ok, "seed pages");
}

static void test_namespace_and_ownership() {
    auto database = memory();
    const std::string owner("\0PLP\1owner\xff", 11);
    seed(database, owner, 2);
    seed(database, "", 1);
    require(database->set("PLP\1", "ordinary"), "prefix key set");
    require(read_page(database, owner, 1) == "page:1" &&
        read_page(database, "", 1) == "page:1", "binary/empty owner");
    std::vector<std::string> keys;
    unsigned long long next = 99;
    require(database->scan_keys(0, 100, keys, next) == db::scan_status::ok &&
        next == 0 && std::set<std::string>(keys.begin(), keys.end()) ==
            std::set<std::string>({owner, "", "PLP\1"}),
        "hidden pages leaked into key namespace");
    std::string snapshot;
    require(database->transact_pages({owner}, [&](page_transaction& view) {
        require(view.lookup(owner, 1, snapshot) == lookup_status::found,
            "owning snapshot read");
        require(view.put(owner, 1, "new"), "stage overwrite");
        std::string staged;
        require(view.lookup(owner, 1, staged) == lookup_status::found &&
            staged == "new", "read own write");
        require(view.erase(owner, 2), "stage erase");
        require(view.lookup(owner, 2, staged) == lookup_status::missing &&
            staged == "new", "read own deletion");
        require(view.put(owner, 2, "revived"), "restage erased page");
        return true;
    }) == batch_status::ok, "staged overwrite commit");
    require(snapshot == "page:1" && read_page(database, owner, 1) == "new" &&
        read_page(database, owner, 2) == "revived", "page ownership");
    read_page(database, owner, 3, false);
    read_page(database, "PLP\1", 1, false);
}

static void test_validation_and_abort() {
    auto database = memory();
    seed(database, "owner", 1);
    require(database->transact_pages({"owner"}, [&](page_transaction& view) {
        put_root(view, "owner", "changed");
        require(view.put("owner", 1, "changed"), "abort stage");
        return false;
    }) == batch_status::aborted, "callback abort ignored");
    require(read_page(database, "owner", 1) == "page:1", "abort changed page");
    for (unsigned mode = 0; mode < 5; ++mode) {
        require(database->transact_pages({"owner"},
            [&](page_transaction& view) {
                put_root(view, "owner", "changed");
                if (mode == 0) {
                    require(!view.put("outside", 1, "x"), "unlocked owner");
                } else if (mode == 1) {
                    require(!view.erase("owner", 0), "zero page id");
                } else if (mode == 2) {
                    require(!view.put("owner", 1, std::string(16385, 'x')),
                        "oversize page");
                } else if (mode == 3) {
                    put_root(view, "owner", std::string(16385, 'x'));
                } else {
                    view.roots().find("owner")->action = action::erase;
                    require(view.put("owner", 1, "orphan"), "orphan stage");
                }
                return true;
            }) == batch_status::aborted, "invalid transaction committed");
        require(read_page(database, "owner", 1) == "page:1",
            "validation failure changed page");
    }
    require(database->transact_pages({"missing"}, [&](page_transaction& view) {
        return view.put("missing", 1, "orphan");
    }) == batch_status::aborted, "page without root accepted");
    bool threw = false;
    try {
        database->transact_pages({"owner"}, [&](page_transaction& view) {
            view.put("owner", 1, "exception");
            throw std::runtime_error("callback");
            return true;
        });
    } catch (const std::runtime_error&) {
        threw = true;
    }
    require(threw && read_page(database, "owner", 1) == "page:1",
        "throwing callback leaked changes/locks");
    require(database->transact_pages({}, [](page_transaction&) {
        return true;
    }) == batch_status::aborted, "empty owner set accepted");
}

static void test_cleanup() {
    for (unsigned mode = 0; mode < 7; ++mode) {
        auto database = memory();
        seed(database, "owner", 3);
        bool ok = false;
        if (mode == 0) {
            ok = database->del("owner");
        } else if (mode == 1) {
            ok = database->set("owner", "overwrite");
        } else if (mode == 2) {
            ok = database->replica_set("owner", "overwrite");
        } else if (mode == 3) {
            ok = database->replica_erase("owner");
        } else if (mode == 4) {
            ok = database->transact({"owner", "other"},
                [](db::batch_view roots) {
                    for (auto& root : roots) {
                        root.action = action::write;
                        root.value = "overwrite";
                    }
                    return true;
                }) == batch_status::ok;
        } else if (mode == 5) {
            ok = database->update("owner", [](bool, std::string&) {
                return action::erase;
            });
        } else {
            ok = database->transact_pages({"owner"},
                [](page_transaction& view) {
                    view.roots().find("owner")->action = action::erase;
                    std::string out = "sentinel";
                    require(view.lookup("owner", 1, out) ==
                        lookup_status::missing && out == "sentinel",
                        "read after staged root deletion");
                    return true;
                }) == batch_status::ok;
        }
        require(ok, "root cleanup operation failed");
        for (uint64_t id = 1; id <= 3; ++id) {
            read_page(database, "owner", id, false);
        }
        seed(database, "owner", 1);
        require(read_page(database, "owner", 1) == "page:1",
            "recreated owner inherited stale pages");
        read_page(database, "owner", 2, false);
    }
}

class failing_memory : public mdb {
public:
    failing_memory() : mdb(2) {}
    bool fail_root = false;
    bool fail_page = false;
    bool oversized_page = false;
protected:
    lookup_status dblookup(const std::string& key,
        std::string& out) override {
        return fail_root ? lookup_status::error : db::dblookup(key, out);
    }
    lookup_status dblookup_page(const std::string& owner, uint64_t id,
        std::string& out) override {
        if (fail_page || oversized_page) {
            out = std::string(oversized_page ? 16385 : 1, 'x');
            return oversized_page ? lookup_status::found : lookup_status::error;
        }
        return mdb::dblookup_page(owner, id, out);
    }
};

static void test_unsupported_and_errors() {
    auto unsupported = db::create_mdb_avl();
    require(unsupported->open(""), "legacy engine open");
    require(unsupported->set("existing", "value"), "legacy engine setup");
    bool called = false;
    require(unsupported->transact_pages({"owner"}, [&](page_transaction&) {
        called = true;
        return true;
    }) == batch_status::unsupported && !called,
        "unsupported engine entered callback");
    std::string legacy;
    require(unsupported->get("existing", legacy) && legacy == "value",
        "unsupported page transaction changed legacy engine");
    auto failing = std::make_shared<failing_memory>();
    require(failing->open(""), "failing memory open");
    seed(failing, "owner", 1);
    failing->fail_root = true;
    require(failing->transact_pages({"owner"}, [&](page_transaction&) {
        called = true;
        return true;
    }) == batch_status::error && !called,
        "failed root lookup entered callback");
    failing->fail_root = false;
    failing->fail_page = true;
    require(failing->transact_pages({"owner"}, [&](page_transaction& view) {
        put_root(view, "owner", "changed");
        std::string out = "sentinel";
        require(view.lookup("owner", 1, out) == lookup_status::error &&
            out == "sentinel", "failed page lookup changed output");
        return true;
    }) == batch_status::error, "failed page lookup committed");
    failing->fail_page = false;
    failing->oversized_page = true;
    require(failing->transact_pages({"owner"}, [&](page_transaction& view) {
        std::string out = "sentinel";
        require(view.lookup("owner", 1, out) == lookup_status::error &&
            out == "sentinel", "oversize stored page accepted");
        return true;
    }) == batch_status::error, "oversize page not treated as storage error");
    failing->oversized_page = false;
    require(read_page(failing, "owner", 1) == "page:1", "error changed data");
    db_watchers watchers;
    auto observed = db::create_mdb(2);
    require(observed->open("", &watchers), "observed memory open");
    require(observed->transact_pages({"owner"}, [&](page_transaction& view) {
        put_root(view, "owner", "root");
        return view.put("owner", 1, "page");
    }) == batch_status::unsupported, "pages published as plain root event");
    std::string out;
    require(!observed->get("owner", out), "observer rejection wrote root");
    require(observed->transact_pages({"owner"}, [](page_transaction&) {
        return true;
    }) == batch_status::ok, "observer prevented read-only callback");
}

static void test_allocation_failures() {
    for (bool commit_only : {false, true}) {
        bool succeeded = false;
        size_t failures = 0;
        for (long budget = 0; budget < 512; ++budget) {
            auto database = memory();
            seed(database, "old", 3);
            seed(database, "erased", 2);
            std::vector<std::string> names = {"old", "new", "erased"};
            const db::page_callback callback = [&](page_transaction& view) {
                    put_root(view, "old", "new-root");
                    put_root(view, "new", "new-root");
                    view.roots().find("erased")->action = action::erase;
                    view.put("old", 1, std::string(1000, 'r'));
                    view.erase("old", 2);
                    view.put("old", 4, std::string(1000, 'a'));
                    for (uint64_t id = 1; id <= 8; ++id) {
                        view.put("new", id, std::string(1000, 'n'));
                    }
                    if (commit_only) {
                        allocation_budget = budget;
                    }
                    return true;
                };
            if (!commit_only) {
                allocation_budget = budget;
            }
            const auto result = database->transact_pages(std::move(names),
                callback);
            allocation_budget = -1;
            if (result == batch_status::error) {
                ++failures;
                std::string root;
                require(database->get("old", root) && root == "root" &&
                    !database->get("new", root) &&
                    database->get("erased", root) && root == "root",
                    "allocation failure exposed root changes");
                for (uint64_t id = 1; id <= 3; ++id) {
                    require(read_page(database, "old", id) ==
                        "page:" + std::to_string(id),
                        "allocation failure exposed page changes");
                }
                read_page(database, "old", 4, false);
                for (uint64_t id = 1; id <= 8; ++id) {
                    read_page(database, "new", id, false);
                }
                require(read_page(database, "erased", 1) == "page:1",
                    "allocation failure reclaimed old pages");
            } else {
                require(result == batch_status::ok, "allocation status");
                require(read_page(database, "old", 1) ==
                    std::string(1000, 'r'), "replacement omitted");
                read_page(database, "old", 2, false);
                read_page(database, "erased", 1, false);
                require(read_page(database, "new", 8) ==
                    std::string(1000, 'n'), "insertion omitted");
                succeeded = true;
                break;
            }
        }
        require(succeeded && failures > 0, "allocation sweep incomplete");
    }
}

static bool increment_pair(page_transaction& view) {
    std::string left, right;
    require(view.lookup("left", 1, left) == lookup_status::found &&
        view.lookup("right", 1, right) == lookup_status::found,
        "concurrent page lookup");
    require(left == right && left == view.roots().find("left")->value &&
        left == view.roots().find("right")->value,
        "torn root/page/multi-key snapshot");
    acl::fiber::yield();
    const auto next = std::to_string(std::stoll(left) + 1);
    put_root(view, "left", next);
    put_root(view, "right", next);
    return view.put("left", 1, next) && view.put("right", 1, next);
}

static void test_concurrent_atomicity() {
    auto database = memory();
    seed(database, "left", 1);
    seed(database, "right", 1);
    require(database->transact_pages({"left", "right"},
        [&](page_transaction& view) {
            put_root(view, "left", "0");
            put_root(view, "right", "0");
            return view.put("left", 1, "0") && view.put("right", 1, "0");
        }) == batch_status::ok, "concurrency setup");
    std::vector<std::thread> workers;
    std::atomic<unsigned> ready{0};
    for (unsigned worker = 0; worker < 4; ++worker) {
        workers.emplace_back([&, worker] {
            ++ready;
            while (ready.load() != 4) {
                std::this_thread::yield();
            }
            for (unsigned fiber = 0; fiber < 4; ++fiber) go[&, worker] {
                for (unsigned i = 0; i < 50; ++i) {
                    const std::vector<std::string> keys = worker % 2 ?
                        std::vector<std::string>{"left", "right", "left"} :
                        std::vector<std::string>{"right", "left"};
                    require(database->transact_pages(keys,
                        increment_pair) == batch_status::ok,
                        "concurrent transaction failed");
                }
            };
            acl::fiber::schedule();
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    require(read_page(database, "left", 1) == "800", "lost concurrent write");
}

static void test_raw_writer_isolation() {
    for (bool erase : {false, true}) {
        auto database = memory();
        seed(database, "owner", 1);
        bool entered = false;
        go[&] {
            require(database->transact_pages({"owner"},
                [&](page_transaction& view) {
                    entered = true;
                    acl::fiber::yield();
                    put_root(view, "owner", "paged-root");
                    return view.put("owner", 1, "paged-value");
                }) == batch_status::ok, "yielding page update");
        };
        go[&] {
            while (!entered) {
                acl::fiber::yield();
            }
            require(erase ? database->replica_erase("owner") :
                database->set("owner", "raw-after"),
                "competing raw mutation");
        };
        acl::fiber::schedule();
        std::string out;
        const bool found = database->get("owner", out);
        require(erase ? !found : found && out == "raw-after",
            "raw writer bypassed page transaction key lock");
        read_page(database, "owner", 1, false);
    }
}

static void test_fragmented_list_storage() {
    auto database = memory();
    const std::string owner("list\0owner", 10);
    const std::string member(1024 * 1024, '\x80');
    const auto fragment_bytes = list_page_codec::max_fragment_bytes;
    const auto pages = (member.size() + fragment_bytes - 1) / fragment_bytes;
    require(database->transact_pages({owner}, [&](page_transaction& view) {
        list_root root;
        root.elements = 1;
        root.head = 1;
        root.tail = pages;
        root.expires_at_ms = 4102444800123;
        std::string bytes;
        require(list_page_codec::encode_root(root, bytes) ==
            list_page_status::ok, "stored List root encode");
        put_root(view, owner, bytes);
        size_t offset = 0;
        for (size_t i = 1; i <= pages; ++i) {
            list_page page;
            page.id = i;
            page.previous = i == 1 ? 0 : i - 1;
            page.next = i == pages ? 0 : i + 1;
            list_fragment fragment;
            fragment.total_bytes = member.size();
            fragment.offset = offset;
            fragment.bytes = member.substr(offset, fragment_bytes);
            offset += fragment.bytes.size();
            page.fragments.push_back(std::move(fragment));
            require(list_page_codec::encode_page(page, bytes) ==
                list_page_status::ok && view.put(owner, i, bytes),
                "bounded List page stage");
        }
        return true;
    }) == batch_status::ok, "fragmented List storage commit");
    std::string root_bytes;
    require(database->get(owner, root_bytes) && root_bytes.size() == 40,
        "List root contains serialized member");
    size_t recovered = 0;
    require(database->transact_pages({owner}, [&](page_transaction& view) {
        list_root root;
        require(list_page_codec::decode_root(
            view.roots().find(owner)->value, root) == list_page_status::ok &&
            root.elements == 1 && root.tail == pages &&
            root.expires_at_ms == 4102444800123, "stored root roundtrip");
        for (uint64_t id = root.head; id != 0;) {
            std::string bytes;
            list_page page;
            require(view.lookup(owner, id, bytes) == lookup_status::found &&
                bytes.size() <= 16384 && list_page_codec::decode_page(bytes,
                    page) == list_page_status::ok, "stored page roundtrip");
            const auto& fragment = page.fragments.front();
            require(fragment.offset == recovered && fragment.bytes ==
                member.substr(recovered, fragment.bytes.size()),
                "stored member fragment mismatch");
            recovered += fragment.bytes.size();
            id = page.next;
        }
        return true;
    }) == batch_status::ok && recovered == member.size(),
        "fragmented List storage incomplete");
}

static size_t measure_update(size_t pages) {
    auto database = memory();
    seed(database, "owner", pages);
    allocated_bytes = 0;
    measure_allocations = true;
    const auto result = database->transact_pages({"owner"},
        [&](page_transaction& view) {
            put_root(view, "owner", "new-root");
            return view.put("owner", 1, std::string(100, 'x'));
        });
    measure_allocations = false;
    require(result == batch_status::ok, "measured update failed");
    return allocated_bytes;
}

int main() {
    acl::acl_cpp_init();
    test_namespace_and_ownership();
    test_validation_and_abort();
    test_cleanup();
    test_unsupported_and_errors();
    test_allocation_failures();
    test_concurrent_atomicity();
    test_raw_writer_isolation();
    test_fragmented_list_storage();
    const auto small = measure_update(10);
    const auto large = measure_update(5000);
    require(large <= small + 1024 && large < 32768,
        "one-page update copied whole page index");
    std::cout << "Atomic page storage tests passed; one-page allocations "
        << small << "/" << large << " bytes for 10/5000 stored pages\n";
}
