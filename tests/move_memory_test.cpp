#include "stdafx.h"
#include "move_memory_support.h"
#include <chrono>
#include <future>
#include <iostream>
#include <thread>

static void faults(bool tbb) {
    unsigned failed = 0, succeeded = 0;
    for (bool first : {false, true}) for (bool existing : {false, true}) {
        unsigned failures = 0, successes = 0;
        for (long allowance = 0; allowance < 128; ++allowance) {
            memory_fixture files(tbb, first, existing);
            const auto before = image(files.from, family_key);
            const auto target = image(files.to, family_key);
            std::vector<watch_token> watched;
            require(files.from->watch({family_key}, watched) ==
                db::batch_status::ok, "memory WATCH seed");
            unsigned completed = 0;
            auto call = memory_actions(completed);
            bool moved = true;
            family_budget = allowance;
            const auto result = move_memory::run(*files.pool, 0, 1, family_key,
                call, moved);
            family_budget = -1;
            no_page_copy(files);
            if (result == db::batch_status::ok) {
                ++successes;
                require(moved && completed == 1 &&
                    !image(files.from, family_key).found &&
                    image(files.to, family_key) == before &&
                    !watched[0]->valid(unix_time_ms()),
                    "ownership success lost pages/WATCH");
            } else {
                ++failures;
                require(result == db::batch_status::error && !moved &&
                    !completed && image(files.from, family_key) == before &&
                    image(files.to, family_key) == target &&
                    watched[0]->valid(unix_time_ms()),
                    "ownership preparation failed to roll back");
            }
        }
        require(failures && successes, "family fault budgets lack both paths");
        failed += failures; succeeded += successes;
    }
    std::cout << "Family budgets " << tbb << ": failed=" << failed
        << " succeeded=" << succeeded << '\n';
}
static void complexity(bool tbb) {
    size_t baseline_count = 0, baseline_bytes = 0;
    for (size_t pages : {size_t(1), size_t(64), size_t(2048)}) {
        memory_fixture files(tbb, true, false, pages);
        const auto before = image(files.from, family_key);
        unsigned completed = 0;
        auto call = memory_actions(completed);
        bool prepared = false, published = false;
        call.observe = [&](move_memory::step at) {
            if (at == move_memory::step::prepared) {
                prepared = true; family_budget = 0;
            }
            if (at == move_memory::step::published) {
                published = true; family_budget = -1;
            }
        };
        bool moved = false;
        family_allocations = family_bytes = 0; family_measuring = true;
        const auto result = move_memory::run(*files.pool, 0, 1, family_key,
            call, moved);
        family_measuring = false; family_budget = -1;
        require(result == db::batch_status::ok && moved && completed == 1 &&
            prepared && published, "allocation-free family publication");
        no_page_copy(files);
        const auto after = image(files.to, family_key);
        require(after == before && !image(files.from, family_key).found,
            "page allocation identity changed during ownership transfer");
        if (pages == 1) {
            baseline_count = family_allocations; baseline_bytes = family_bytes;
        } else require(family_allocations == baseline_count &&
            family_bytes == baseline_bytes,
            "transfer allocation scales with pages");
        std::cout << "Family ownership " << tbb << " pages=" << pages
            << " allocations=" << family_allocations
            << " bytes=" << family_bytes << '\n';
        // Reverse IDs exercise both native frame orders without copying pages.
        bool reversed = false;
        auto reverse = memory_actions(completed);
        require(move_memory::run(*files.pool, 1, 0, family_key, reverse,
            reversed) == db::batch_status::ok && reversed &&
            image(files.from, family_key) == before &&
            !image(files.to, family_key).found, "reverse ownership transfer");
    }
}
static void typed(bool tbb) {
    memory_fixture files(tbb, true, false, 0);
    unsigned completed = 0;
    auto call = memory_actions(completed);
    typed_recovery(files.from, files.to, [&](const std::string& key) {
        const auto before = image(files.from, key);
        bool moved = false;
        require(move_memory::run(*files.pool, 0, 1, key, call, moved) ==
            db::batch_status::ok && moved && image(files.to, key) == before &&
            !image(files.from, key).found, "typed ownership transfer");
    });
    std::cout << "Typed family ownership " << tbb << " passed\n";
}
static void errors(bool tbb) {
    memory_fixture files(tbb, false, true);
    const auto before = image(files.from, family_key);
    const auto target = image(files.to, family_key);
    unsigned completed = 0;
    auto call = memory_actions(completed);
    bool moved = true;
    for (const auto id : {size_t(0), size_t(2)})
        require(move_memory::run(*files.pool, 0, id, family_key, call, moved)
            == db::batch_status::error && !moved, "invalid memory DB IDs");
    control(files.to).fail_prepare = true;
    require(move_memory::run(*files.pool, 0, 1, family_key, call, moved) ==
        db::batch_status::error && !moved, "second native prepare failure");
    control(files.to).fail_prepare = false;
    control(files.from).fail_read = true;
    require(move_memory::run(*files.pool, 0, 1, family_key, call, moved) ==
        db::batch_status::error && !moved, "bounded root read failure");
    control(files.from).fail_read = false;
    auto missing = call; missing.complete = {};
    require(move_memory::run(*files.pool, 0, 1, family_key, missing, moved) ==
        db::batch_status::error && !moved, "missing publication callback");
    require(image(files.from, family_key) == before &&
        image(files.to, family_key) == target && !completed,
        "preparation error changed family");
    std::string key(family_key);
    call.prepare = [&](const move_memory::roots& view) {
        key = "redirect";
        require(view.owner == family_key, "borrowed owner changed");
        return move_memory::choice::proceed;
    };
    require(move_memory::run(*files.pool, 0, 1, key, call, moved) ==
        db::batch_status::ok && moved &&
        image(files.to, family_key) == before &&
        !image(files.to, "redirect").found, "caller alias redirected family");
}
static void isolation(bool tbb) {
    memory_fixture files(tbb, true, false);
    std::vector<watch_token> watched, target_watched;
    require(files.from->watch({family_key}, watched) == db::batch_status::ok &&
        files.to->watch({family_key}, target_watched) == db::batch_status::ok,
        "paired WATCH seed");
    unsigned completed = 0;
    auto call = memory_actions(completed);
    std::vector<std::future<bool>> writers;
    call.observe = [&](move_memory::step at) {
        if (at == move_memory::step::validated) {
            for (auto database : {files.from, files.to}) {
                auto entered = std::make_shared<std::promise<void>>();
                auto started = entered->get_future();
                writers.push_back(std::async(std::launch::async,
                    [database, entered] {
                    entered->set_value();
                    return database->set(family_key, "after");
                }));
                started.get();
            }
        }
        for (auto& writer : writers)
            require(writer.wait_for(std::chrono::milliseconds(20)) ==
                std::future_status::timeout, "writer entered family transfer");
    };
    call.complete = [&] {
        require(!watched[0]->valid(unix_time_ms()) &&
            !target_watched[0]->valid(unix_time_ms()),
            "publication preceded paired logical effects");
        ++completed;
    };
    bool moved = false;
    require(move_memory::run(*files.pool, 0, 1, family_key, call, moved) ==
        db::batch_status::ok && moved && completed == 1,
        "memory ownership isolation");
    for (auto& writer : writers) require(writer.get(), "waiting writer failed");
}
static void readiness(bool tbb) {
    memory_fixture files(tbb, true, false, 0);
    require(files.from->del(family_key), "replace inline seed");
    dao::paged_list lists;
    long long length = 0;
    require(lists.push(files.from, family_key, {"payload"}, false, false,
        length) && length == 1, "ownership list seed");
    auto source = files.from->wait_for_lists({family_key});
    auto target = files.to->wait_for_lists({family_key});
    auto source_change = files.from->wait_for_keys({family_key});
    auto target_change = files.to->wait_for_keys({family_key});
    unsigned completed = 0;
    auto call = memory_actions(completed);
    call.complete = [&] {
        key_wait_event event;
        require(source_change->wait(0, event) &&
            target_change->wait(0, event) && target->wait(0, event) &&
            event.ready && target->key(event) == family_key,
            "publication preceded paired readiness");
        ++completed;
    };
    bool moved = false;
    require(move_memory::run(*files.pool, 0, 1, family_key, call, moved) ==
        db::batch_status::ok && moved && completed == 1,
        "list family readiness transfer");
    key_wait_event event;
    if (source->wait(0, event)) require(!event.ready,
        "deleted source retained list readiness");
}
static void expiry(bool tbb) {
    memory_fixture files(tbb, false, false);
    require(files.from->rebuild_expiry() == db::scan_status::ok &&
        files.to->rebuild_expiry() == db::scan_status::ok,
        "ownership expiry cache seed");
    control(files.from).reset(); control(files.to).reset();
    int64_t deadline;
    {
        execution_guard frozen(true);
        deadline = unix_time_ms() + 1;
        require(files.from->update_root(family_key,
            [&](bool found, std::string& root, bool& keep) {
            require(found, "expiry source present");
            root = encoded_root(3, deadline); keep = true;
            return db::update_action::write;
        }), "ownership source deadline");
        unsigned completed = 0;
        auto call = memory_actions(completed);
        bool moved = false;
        require(move_memory::run(*files.pool, 0, 1, family_key, call, moved) ==
            db::batch_status::ok && moved, "deadline ownership transfer");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    require(unix_time_ms() > deadline, "exclusive clock was not released");
    size_t removed = 0;
    for (unsigned i = 0; i < 256 && !removed; ++i) {
        const auto step = files.to->expire_step(1);
        require(step.ok && step.supported, "target expiry failed");
        removed += step.removed;
    }
    require(removed == 1 && !image(files.to, family_key).found &&
        !control(files.to).scans && !control(files.from).scans,
        "moved deadline missing from active expiry cache");
}
static void bounds(bool tbb) {
    memory_fixture files(tbb, true, false);
    const std::string oversized(16385, 'x');
    require(files.from->set("oversize", oversized), "legacy root seed");
    unsigned completed = 0;
    auto call = memory_actions(completed);
    call.prepare = [](const move_memory::roots&) {
        require(false, "unbounded root reached policy");
        return move_memory::choice::error;
    };
    bool moved = true;
    family_allocations = family_bytes = 0; family_measuring = true;
    const auto result = move_memory::run(*files.pool, 0, 1, "oversize", call,
        moved);
    family_measuring = false;
    require(result == db::batch_status::error && !moved && !completed &&
        family_bytes < oversized.size(), "root was copied before size check");
    std::string bytes;
    require(files.from->get("oversize", bytes) && bytes == oversized &&
        !files.to->get("oversize", bytes), "rejected root changed storage");
    require(files.from->transact_pages({family_key}, [&](page_transaction&) {
        return control(files.from).visit(family_key,
            [](uint64_t, const std::string&) { return false; }) ==
            db::batch_status::aborted;
    }) == db::batch_status::ok, "borrowed visitor early stop");
}
static void unsupported() {
    auto from = memory_database(false, 4);
    auto workspace = std::make_shared<page_workspace>(from);
    require(from->open(""), "unsupported memory open");
    database_pool pool(from, 2, [workspace](size_t id) {
        return id == 1 ? workspace : shared_db();
    });
    unsigned completed = 0;
    auto call = memory_actions(completed);
    call.prepare = [](const move_memory::roots&) {
        require(false, "unsupported family called policy");
        return move_memory::choice::error;
    };
    bool moved = true;
    require(move_memory::run(pool, 0, 1, family_key, call, moved) ==
        db::batch_status::unsupported && !moved, "workspace family support");
#ifdef HAS_TBB
    auto tbb = memory_database(true, 4); require(tbb->open(""), "mixed open");
    database_pool mixed(from, 2, [tbb](size_t id) {
        return id == 1 ? tbb : shared_db();
    });
    require(move_memory::run(mixed, 0, 1, family_key, call, moved) ==
        db::batch_status::unsupported && !moved, "mixed memory family support");
#endif
}
static void injected(bool tbb, bool first, const std::string& point) {
    memory_fixture files(tbb, first, false);
    const auto before = image(files.from, family_key);
    const auto target = image(files.to, family_key);
    unsigned completed = 0;
    auto call = memory_actions(completed);
    const auto fail = [&] {
        std::cerr << "FAULT " << point << '\n'; throw std::bad_alloc();
    };
    call.observe = [&](move_memory::step at) {
        const char* names[] = {"validated", "prepared", "published",
            "completed"};
        if (point == names[static_cast<unsigned>(at)]) fail();
    };
    call.complete = [&] { if (point == "complete") fail(); ++completed; };
    bool moved = true;
    require(move_memory::run(*files.pool, 0, 1, family_key, call, moved) ==
        db::batch_status::error && !moved && !completed &&
        image(files.from, family_key) == before &&
        image(files.to, family_key) == target, "early exception rollback");
}
static void inline_families(bool tbb) {
    for (bool first : {false, true}) for (bool existing : {false, true}) {
        for (const auto& key : {std::string(), family_key}) {
            for (const auto& value : {std::string(),
                std::string("a\0\xff", 3)}) {
                memory_fixture files(tbb, first, false, 0);
                if (key != family_key)
                    require(files.from->del(family_key), "inline seed reset");
                string_root root;
                root.length = value.size(); root.inline_value = value;
                root.expires_at_ms = family_deadline;
                std::string encoded;
                require(string_page_codec::encode_root(root, encoded) ==
                    string_page_status::ok && files.from->set(key, encoded),
                    "inline binary value seed");
                if (existing) seed(files.to, key, 5, 1);
                files.events.calls = 0;
                control(files.from).reset(); control(files.to).reset();
                const auto before = image(files.from, key);
                unsigned completed = 0;
                auto call = memory_actions(completed);
                call.observe = [](move_memory::step at) {
                    if (at == move_memory::step::prepared) family_budget = 0;
                    if (at == move_memory::step::published) family_budget = -1;
                };
                bool moved = false;
                require(move_memory::run(*files.pool, 0, 1, key, call, moved)
                    == db::batch_status::ok && moved && completed == 1 &&
                    !image(files.from, key).found &&
                    image(files.to, key) == before,
                    "inline/empty-key family transfer");
                no_page_copy(files);
                require(move_memory::run(*files.pool, 1, 0, key, call, moved)
                    == db::batch_status::ok && moved && completed == 2 &&
                    image(files.from, key) == before &&
                    !image(files.to, key).found, "inline reverse transfer");
                index(files.from, {key}); index(files.to, {});
            }
        }
    }
    std::cout << "Inline/empty-key family ownership " << tbb << " passed\n";
}
int main(int argc, char** argv) {
    const struct rlimit no_core = {0, 0}; ::setrlimit(RLIMIT_CORE, &no_core);
    acl::acl_cpp_init();
    if (argc == 5) {
        injected(std::string(argv[1]) == "tbb", std::string(argv[2]) == "first",
            argv[3]);
        return 0;
    }
    require(argc == 1, "memory ownership arguments");
    for (const bool tbb : {false
#ifdef HAS_TBB
        , true
#endif
    }) {
        faults(tbb); complexity(tbb); typed(tbb); errors(tbb); isolation(tbb);
        readiness(tbb); expiry(tbb); bounds(tbb);
        inline_families(tbb);
    }
    unsupported();
    std::cout << "Memory family ownership, rollback "
        "and logical effects passed\n";
}
