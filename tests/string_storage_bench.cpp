#include "stdafx.h"
#include "dao/paged_string.h"
#include "dao/json/json_string.h"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <iomanip>

using namespace pkv;
static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
static void measure(bool tbb, bool pages, size_t size, bool append,
    unsigned trial) {
    auto database = tbb ? db::create_mdb_tbb(16) : db::create_mdb(16);
    require(database && database->open(""), "benchmark backend open");
    std::unique_ptr<dao::string> data;
    if (pages) data.reset(new dao::paged_string);
    else data.reset(new dao::json_string);
    const std::string initial(size, 'x');
    require(data->set(database, "bench", initial), "benchmark seed");
    const unsigned iterations = append ? 30 : (size <= 1024 ? 1000 : 100);
    const auto operation = [&](unsigned i) {
        if (append) {
            long long length;
            return data->append(database, "bench", "x", length) &&
                length == static_cast<long long>(size + i + 1);
        }
        std::string out;
        return data->getrange(database, "bench", size / 2, size / 2 + 1,
            out) && out == "xx";
    };
    if (!append) for (unsigned i = 0; i < 5; ++i)
        require(operation(i), "benchmark warmup");
    const auto begin = std::chrono::steady_clock::now();
    for (unsigned i = 0; i < iterations; ++i)
        require(operation(i), "benchmark operation");
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - begin).count();
    std::string value;
    require(data->get(database, "bench", value) &&
        value.size() == size + (append ? iterations : 0) &&
        value.find_first_not_of('x') == std::string::npos,
        "benchmark final bytes");
    std::cout << trial << ',' << (tbb ? "oneTBB" : "mdb") << ',' <<
        (pages ? "paged_string" : "legacy_json") << ',' << size << ',' <<
        (append ? "append_1" : "getrange_2") << ',' << iterations << ',' <<
        std::fixed << std::setprecision(2) <<
        static_cast<double>(elapsed) / iterations << '\n';
}
int main() {
    acl::acl_cpp_init();
    std::cout << "trial,backend,dao,initial_bytes,operation,iterations,ns_op\n";
    for (unsigned trial = 1; trial <= 3; ++trial)
        for (bool tbb : {false, true}) {
#ifndef HAS_TBB
            if (tbb) continue;
#endif
            for (size_t size : {1024U, 1024U * 1024U, 8U * 1024U * 1024U})
                for (bool append : {false, true})
                    for (unsigned order = 0; order < 2; ++order)
                        measure(tbb, (trial + order) % 2 != 0, size, append,
                            trial);
        }
}
