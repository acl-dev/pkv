#include "stdafx.h"
#include "db/mdb/mdb.h"
#include "db/page_transaction.h"
#include <cstdlib>
#include <iostream>
#include <new>
static thread_local bool measuring = false;
static thread_local size_t allocations = 0;
void* operator new(std::size_t bytes) {
    if (measuring) ++allocations;
    if (void* p = std::malloc(bytes ? bytes : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
#if __cplusplus >= 201402L
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
#endif
// Private engine, no competing actor; count native commit allocations only.
class native_db : public pkv::mdb {
public:
    native_db() : mdb(4) {}
    batch_status apply(const std::vector<batch_entry>& roots) {
        return dbapply_pages(roots, {});
    }
};
static void require(bool ok, const char* reason) {
    if (!ok) { std::cerr << reason << '\n'; std::exit(1); }
}
int main() {
    acl::acl_cpp_init();
    for (bool erase : {false, true}) {
        native_db db;
        if (!db.open("") || !db.set("key", std::string(128, 'a'))) return 1;
        std::vector<pkv::db::batch_entry> roots;
        roots.emplace_back("key", true, std::string(128, 'b'));
        roots[0].action = erase ? pkv::db::update_action::erase :
            pkv::db::update_action::write;
        allocations = 0; measuring = true;
        const auto status = db.apply(roots);
        measuring = false;
        require(status == pkv::db::batch_status::ok, "native commit");
        require(allocations == (erase ? 2U : 4U),
            "unused rollback buffers allocated");
        std::string out;
        require(db.get("key", out) == !erase &&
            (erase || out == std::string(128, 'b')), "native commit bytes");
        uint64_t count = 99;
        require(db.key_count(count) == pkv::db::count_status::ok &&
            count == unsigned(!erase), "native commit roots");
        std::cout << (erase ? "erase" : "overwrite") << ": " <<
            allocations << '\n';
    }
}
