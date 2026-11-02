#pragma once
#include "replica_group_support.h"
#include <thread>

static std::shared_ptr<const page_batch> capture_family(shared_db database,
    unsigned id, const std::string& key) {
    execution_guard admission(true);
    std::shared_ptr<const page_batch> result;
    require(database->transact_pages({key}, [&](page_transaction& tx) {
        const auto& source = *tx.roots().find(key);
        if (!source.found) return false;
        std::vector<db::batch_entry> roots;
        roots.emplace_back(key, true, source.value);
        roots.back().action = db::update_action::write;
        roots.back().clear_pages = true;
        std::vector<page_change> pages;
        if (control(database).visit(key, [&](uint64_t page,
            const std::string& bytes) {
            pages.emplace_back(key, page, bytes, db::update_action::write);
            return true;
        }) != db::batch_status::ok) return false;
        std::sort(pages.begin(), pages.end(), [](const page_change& a,
            const page_change& b) { return a.id < b.id; });
        result = page_batch::capture(id, roots, pages);
        return result != nullptr;
    }) == db::batch_status::ok, "typed group family capture");
    return result;
}
static void typed_group(bool tbb) {
    group_fixture files(tbb);
    typed_recovery(files.databases[0], files.databases[1],
        [&](const std::string& key) {
        const auto source = image(files.databases[0], key);
        auto payload = capture_family(files.databases[0], 7, key);
        auto waiting = files.databases[1]->wait_for_lists({key});
        const auto input = exec_batch::capture({group_erase(0, key), payload,
            group_write(15, family_key, 2)});
        files.events.calls = 0;
        require(input && input->apply(*files.pool) == db::batch_status::ok,
            "typed group apply");
        const auto target = image(files.databases[1], key);
        require(!image(files.databases[0], key).found &&
            source.root == target.root &&
            source.pages.size() == target.pages.size() && !files.events.calls,
            "typed group root or page count changed");
        for (const auto& page : source.pages) {
            const auto found = target.pages.find(page.first);
            require(found != target.pages.end() &&
                found->second.bytes == page.second.bytes &&
                found->second.crc == page.second.crc, "typed group page bytes");
        }
        key_wait_event event;
        if (key == "list") require(waiting->wait(0, event) && event.ready &&
            waiting->key(event) == key, "group target List readiness missing");
    });
    std::cout << "Typed bounded group replay " << tbb << " passed\n";
}
static void expiry_group(bool tbb) {
    group_fixture files(tbb);
    for (auto database : files.databases) {
        require(database->rebuild_expiry() == db::scan_status::ok,
            "group expiry index seed"); control(database).reset();
    }
    {
        execution_guard frozen(true);
        std::vector<db::batch_entry> roots;
        roots.emplace_back(family_key, true,
            encoded_root(1, unix_time_ms() + 1));
        roots.back().action = db::update_action::write;
        roots.back().clear_pages = true;
        const auto deadline = page_batch::capture(7, roots,
            {{family_key, 1, page_bytes(1), db::update_action::write}});
        const auto input = exec_batch::capture({group_erase(0, family_key),
            deadline, group_write(15, family_key, 2)});
        require(input && input->apply(*files.pool) == db::batch_status::ok,
            "group expiry apply");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const auto expired = files.databases[1]->expire_step(16);
    require(expired.ok && expired.supported && expired.removed == 1 &&
        !image(files.databases[1], family_key).found,
        "group target active expiry lost");
    for (auto database : files.databases)
        require(!control(database).scans, "group expiry rebuilt/scanned DB");
    std::cout << "Indexed group expiry " << tbb << " passed\n";
}
static void colliding_stripes(bool tbb) {
    group_fixture files(tbb);
    std::map<std::string, std::string> values;
    for (unsigned i = 0; i < 300; ++i)
        values.emplace("owner" + std::to_string(i), encoded_root(0, -1));
    std::vector<db::batch_entry> roots;
    std::set<std::string> keys{family_key};
    for (const auto& value : values) {
        roots.emplace_back(value.first, false, value.second);
        roots.back().action = db::update_action::write;
        roots.back().clear_pages = true; keys.insert(value.first);
    }
    const auto input = exec_batch::capture({page_batch::capture(0, roots, {}),
        group_write(7, family_key, 1), group_write(15, family_key, 2)});
    require(input && input->apply(*files.pool) == db::batch_status::ok,
        "colliding key stripes did not commit");
    index(files.databases[0], keys);
    std::cout << "Deduplicated 300-owner group stripes " << tbb << " passed\n";
}
static void unsupported_group(bool tbb) {
    group_fixture files(tbb); const auto before = files.images();
    std::vector<shared_db> unsupported;
    unsupported.push_back(std::make_shared<page_workspace>(files.databases[1]));
#ifdef HAS_ROCKSDB
    unsupported.push_back(std::make_shared<rdb>());
#endif
#ifdef HAS_WT
    unsupported.push_back(std::make_shared<wdb>());
#endif
    for (const auto& database : unsupported) {
        database_pool pool(files.databases[0], 16, [&](size_t id) {
            return id == 7 ? database : files.databases[2];
        });
        require(files.group->apply(pool) == db::batch_status::unsupported &&
            !files.trace.prepared && files.images() == before,
            "unsupported group applied a memory prefix");
    }
    auto unopened = group_database(tbb, files.trace, 1);
    database_pool pool(files.databases[0], 16, [&](size_t id) {
        return id == 7 ? unopened : files.databases[2];
    });
    require(files.group->apply(pool) == db::batch_status::error &&
        !files.trace.prepared && files.images() == before,
        "unopened memory group accepted");
}
