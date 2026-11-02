#include "stdafx.h"
#include "db/db.h"
#include <cstdlib>
#include <new>
#include <iostream>
#include <set>

// Limit allocations only inside native commit. No production fault-injection
// switches are needed; every allocation point up to success is exercised.
static thread_local long allocation_budget = -1;
void* operator new(std::size_t size) {
    if (allocation_budget == 0) throw std::bad_alloc();
    if (allocation_budget > 0) --allocation_budget;
    if (void* ptr = std::malloc(size ? size : 1)) return ptr;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* ptr) noexcept { std::free(ptr); }
void operator delete[](void* ptr) noexcept { std::free(ptr); }
#if __cplusplus >= 201402L
void operator delete(void* ptr, std::size_t) noexcept { std::free(ptr); }
void operator delete[](void* ptr, std::size_t) noexcept { std::free(ptr); }
#endif

static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}

int main() {
    acl::acl_cpp_init();
    bool succeeded = false;
    size_t failures = 0;
    for (long fail_at = 0; fail_at < 512; ++fail_at) {
        auto db = pkv::db::create_mdb(8);
        require(db->open(""), "memory open failed");
        std::vector<std::string> keys;
        for (int i = 0; i < 24; ++i) {
            const auto key = "batch-key-" + std::to_string(i);
            keys.push_back(key);
            if (i < 16) require(db->set(key, "original"), "fixture setup failed");
        }
        const auto status = db->transact(keys, [&](pkv::db::batch_view entries) {
            size_t i = 0;
            for (auto& entry : entries) {
                if (i++ < 3) entry.action = pkv::db::update_action::erase;
                else {
                    entry.value.assign(1024, 'x');
                    entry.action = pkv::db::update_action::write;
                }
            }
            allocation_budget = fail_at;
            return true;
        });
        allocation_budget = -1;
        require(status == pkv::db::batch_status::ok || status == pkv::db::batch_status::error,
                "unexpected native batch status");
        if (status == pkv::db::batch_status::error) {
            ++failures;
            for (size_t i = 0; i < keys.size(); ++i) {
                std::string value;
                const bool found = db->get(keys[i], value);
                require(found == (i < 16) && (!found || value == "original"), "failed batch exposed partial commit");
            }
        } else {
            succeeded = true;
            auto ordered = keys;
            std::sort(ordered.begin(), ordered.end());
            for (size_t i = 0; i < ordered.size(); ++i) {
                std::string value;
                const bool found = db->get(ordered[i], value);
                require(i < 3 ? !found : found && value == std::string(1024, 'x'), "successful batch omitted an action");
            }
            break;
        }
    }
    require(succeeded && failures > 0, "allocation failure loop did not exercise failure and success");
    auto scan_db = pkv::db::create_mdb(8);
    require(scan_db->open(""), "scan failure fixture open failed");
    std::set<std::string> expected_keys;
    for (int i = 0; i < 64; ++i) {
        const auto key = std::string(80, 'k') + std::to_string(i);
        expected_keys.insert(key);
        require(scan_db->set(key, "original"), "scan failure fixture write failed");
    }
    size_t scan_failures = 0;
    bool scan_succeeded = false;
    for (long fail_at = 0; fail_at < 256; ++fail_at) {
        std::vector<std::string> page;
        unsigned long long next = 99;
        allocation_budget = fail_at;
        const auto status = scan_db->scan_keys(0, 10, page, next);
        allocation_budget = -1;
        require(status == pkv::db::scan_status::error || status == pkv::db::scan_status::ok, "unexpected native scan status");
        if (status == pkv::db::scan_status::error) {
            ++scan_failures;
            require(page.empty() && next == 0, "failed scan exposed partial keys/cursor");
        } else {
            require(page.size() >= 10 && std::set<std::string>(page.begin(), page.end()).size() == page.size(), "successful scan omitted/duplicated page keys");
            scan_succeeded = true;
        }
        std::vector<std::string> all;
        require(scan_db->scan_keys(0, 1000, all, next) == pkv::db::scan_status::ok &&
                std::set<std::string>(all.begin(), all.end()) == expected_keys, "scan allocation failure changed stored keys");
        if (scan_succeeded) break;
    }
    require(scan_succeeded && scan_failures > 0, "scan allocation failures not exercised");
    std::cout << "Atomic batch preserved all values across " << failures << " allocation failure points\n";
    std::cout << "Native scan preserved all keys across " << scan_failures << " allocation failure points\n";
}
