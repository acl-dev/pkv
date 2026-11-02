#include "stdafx.h"
#include "dao/paged_hash.h"
#include "dao/json/json_hash.h"
#include "common/redis_integer.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <thread>

using namespace pkv;

static void require(bool ok, const char* reason) {
    if (!ok) { std::cerr << reason << '\n'; std::exit(1); }
}

static std::unique_ptr<dao::hash> access(bool paged) {
    return std::unique_ptr<dao::hash>(paged ?
        static_cast<dao::hash*>(new dao::paged_hash) : new dao::json_hash);
}

static void work(dao::hash& hash, shared_db& database, const std::string& key,
    size_t iterations) {
    for (size_t i = 0; i < iterations; ++i) {
        long long value;
        std::string out;
        require(hash.hincrby(database, key, "count", 1, value) &&
            hash.hget(database, key, "probe", out) && out == "v",
            "benchmark command failed");
    }
}

static double sample(bool tbb, bool paged, size_t threads, size_t iterations,
    size_t field_count) {
    auto database = tbb ? db::create_mdb_tbb(16) : db::create_mdb(16);
    require(database && database->open(""), "benchmark open");
    std::map<std::string, std::string> fields;
    for (size_t i = 0; i < field_count; ++i) fields[std::to_string(i)] = "v";
    fields["count"] = "0"; fields["probe"] = "v";
    std::vector<std::string> keys;
    auto writer = access(paged);
    for (size_t t = 0; t < threads; ++t) {
        keys.push_back("worker:" + std::to_string(t));
        require(writer->hmset(database, keys.back(), fields) ==
            static_cast<int>(fields.size()), "benchmark seed");
    }
    std::atomic<size_t> ready(0);
    std::atomic<bool> start(false);
    std::vector<std::thread> workers;
    for (size_t t = 0; t < threads; ++t) workers.emplace_back([&, t] {
        auto hash = access(paged);
        work(*hash, database, keys[t], 20);
        ++ready;
        while (!start.load()) std::this_thread::yield();
        work(*hash, database, keys[t], iterations);
    });
    while (ready != threads) std::this_thread::yield();
    const auto begin = std::chrono::steady_clock::now();
    start = true;
    for (auto& worker : workers) worker.join();
    const double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - begin).count();
    for (const auto& key : keys) {
        std::string value;
        require(writer->hget(database, key, "count", value) &&
            value == std::to_string(iterations + 20), "lost benchmark writes");
    }
    return static_cast<double>(threads * iterations * 2) / elapsed;
}

int main(int argc, char** argv) {
    acl::acl_cpp_init();
    long long iterations = 500, fields = 1024;
    require(argc <= 3 && (argc < 2 || parse_redis_integer(argv[1], iterations))
        && (argc < 3 || parse_redis_integer(argv[2], fields)) && iterations > 0
        && iterations <= 100000 && fields > 0 && fields <= 100000,
        "usage: memory_backend_bench [iterations<=100000] [fields<=100000]");
    std::cout << "engine,format,threads,fields,iterations,median_ops_sec,"
        "min_ops_sec,max_ops_sec\n";
    for (size_t threads : {size_t(1), size_t(4)}) {
        for (bool tbb : {false, true}) {
            for (bool paged : {false, true}) {
                std::vector<double> samples;
                for (unsigned i = 0; i < 3; ++i)
                    samples.push_back(sample(tbb, paged, threads,
                        static_cast<size_t>(iterations),
                        static_cast<size_t>(fields)));
                std::sort(samples.begin(), samples.end());
                std::cout << (tbb ? "oneTBB" : "mdb") << ',' <<
                    (paged ? "paged_hash" : "legacy_json") << ',' << threads <<
                    ',' << fields << ',' << iterations << ',' << std::fixed <<
                    std::setprecision(1) << samples[1] << ',' << samples[0] <<
                    ',' << samples[2] << std::endl;
            }
        }
    }
}
