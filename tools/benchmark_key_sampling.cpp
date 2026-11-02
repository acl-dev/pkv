#include "stdafx.h"
#include "db/mdb/mdb.h"
#include "db/mdb/mdb_tbb.h"
#include "common/set_sampling.h"
#include <chrono>
#include <cstdlib>
#include <iostream>

using namespace pkv;
template<class Backend> class measured : public Backend {
public:
    measured() : Backend(4) {}
    bool visitor = false;
protected:
    db::scan_status dbkey_at_rank(uint64_t rank, std::string& out,
        bool& found) override {
        return visitor ? db::dbkey_at_rank(rank, out, found) :
            Backend::dbkey_at_rank(rank, out, found);
    }
};
template<class Backend> void run(const char* name, size_t size) {
    measured<Backend> database;
    if (!database.open("")) std::exit(1);
    for (size_t i = 0; i < size; ++i)
        if (!database.set("key:" + std::to_string(i), "opaque-root"))
            std::exit(1);
    // Raw stored-key selection only. No record decode, sockets or payload IO.
    const size_t samples = 1024;
    std::string key;
    key.reserve(64);
    bool found = false;
    if (database.sample_key(key, found) != db::scan_status::ok || !found)
        std::exit(1);
    for (unsigned round = 0; round < 3; ++round) {
        const auto sequence = set_sampling_engine();
        uint64_t expected = 0;
        for (unsigned order = 0; order < 2; ++order) {
            set_sampling_engine() = sequence;
            database.visitor = ((round + order) % 2) != 0;
            const auto started = std::chrono::steady_clock::now();
            size_t checked = 0;
            uint64_t digest = 0;
            for (; checked < samples; ++checked) {
                if (database.sample_key(key, found) != db::scan_status::ok ||
                    !found || key.compare(0, 4, "key:")) std::exit(1);
                digest = (digest * 131) ^ std::hash<std::string>()(key);
            }
            const auto elapsed = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - started).count();
            if (!order) expected = digest;
            else if (expected != digest) std::exit(1);
            std::cout << "{\"backend\":\"" << name << "\",\"roots\":" << size
                << ",\"visitor\":" << (database.visitor ? "true" : "false")
                << ",\"round\":" << round << ",\"samples\":" << checked
                << ",\"seconds\":" << elapsed << "}\n";
        }
    }
}
int main() {
    acl::acl_cpp_init();
    for (size_t size : {1024U, 16384U, 131072U}) {
        run<mdb>("mdb", size);
#ifdef HAS_TBB
        run<mdb_tbb>("tbb", size);
#endif
    }
}
