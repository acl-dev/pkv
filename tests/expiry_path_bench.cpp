#include "stdafx.h"
#include "db/expiry_index.h"
#include "dao/paged_string.h"
#include "coder/string_page.h"
#include "common/redis_integer.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <thread>

using namespace pkv;
static const size_t key_count = 256;
static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
static std::string root(const std::string& value, int64_t deadline = -1) {
    string_root metadata;
    metadata.length = value.size(); metadata.inline_value = value;
    metadata.expires_at_ms = deadline;
    std::string bytes;
    require(string_page_codec::encode_root(metadata, bytes) ==
        string_page_status::ok, "benchmark root encode"); return bytes;
}
static std::vector<std::vector<std::string>> names(size_t threads) {
    std::vector<std::vector<std::string>> out(threads);
    for (size_t t = 0; t < threads; ++t)
        for (size_t key = 0; key < key_count; ++key)
            out[t].push_back("persistent:" + std::to_string(t) + ':' +
                std::to_string(key));
    return out;
}
template<class Work>
static double parallel(size_t threads, size_t iterations, Work work) {
    std::atomic<size_t> ready(0);
    std::atomic<bool> start(false);
    std::vector<std::thread> workers;
    for (size_t t = 0; t < threads; ++t) workers.emplace_back([&, t] {
        for (size_t i = 0; i < 64; ++i) require(work(t, i), "warmup failed");
        ++ready;
        while (!start.load(std::memory_order_acquire))
            std::this_thread::yield();
        for (size_t i = 0; i < iterations; ++i)
            require(work(t, i), "benchmark operation failed");
    });
    while (ready.load() != threads) std::this_thread::yield();
    const auto begin = std::chrono::steady_clock::now();
    start.store(true, std::memory_order_release);
    for (auto& worker : workers) worker.join();
    const auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - begin).count();
    return static_cast<double>(threads * iterations) / elapsed;
}
static void row(unsigned trial, const char* engine, const char* layer,
    const char* state, size_t threads, size_t iterations, double rate) {
    std::cout << trial << ',' << engine << ',' << layer << ',' << state << ','
        << threads << ',' << iterations << ',' << std::fixed <<
        std::setprecision(1) << rate << std::endl;
}
static void store_sample(unsigned trial, bool tbb, bool paged, unsigned state,
    size_t threads, size_t iterations) {
    auto database = tbb ? db::create_mdb_tbb(16) : db::create_mdb(16);
    require(database && database->open(""), "benchmark DB open");
    const auto keys = names(threads);
    const std::string values[] = {std::string(128, 'a'), std::string(128, 'b')};
    const std::string roots[] = {root(values[0]), root(values[1])};
    for (const auto& worker : keys) for (const auto& key : worker)
        require(database->set(key, roots[0]), "benchmark root seed");
    if (state == 2) {
        const auto expiring = root("future", 4102444800123LL);
        for (size_t i = 0; i < key_count; ++i)
            require(database->set("ttl:" + std::to_string(i), expiring),
                "benchmark TTL seed");
    }
    if (state) require(database->rebuild_expiry() == db::scan_status::ok,
        "benchmark index snapshot");
    std::vector<std::unique_ptr<dao::paged_string>> data;
    for (size_t t = 0; t < threads; ++t)
        data.emplace_back(new dao::paged_string);
    const auto rate = parallel(threads, iterations, [&](size_t t, size_t i) {
        const auto& key = keys[t][i % key_count];
        const auto version = (i / key_count) % 2;
        return paged ? data[t]->set(database, key, values[version]) :
            database->set(key, roots[version]);
    });
    const auto last = iterations - 1;
    for (size_t t = 0; t < threads; ++t) {
        std::string value;
        require(data[t]->get(database, keys[t][last % key_count], value) &&
            value == values[(last / key_count) % 2], "benchmark final bytes");
    }
    uint64_t total;
    require(database->key_count(total) == db::count_status::ok &&
        total == threads * key_count + (state == 2 ? key_count : 0),
        "benchmark root count");
    const char* states[] = {"disabled", "empty", "future"};
    row(trial, tbb ? "oneTBB" : "mdb", paged ? "paged_set" : "raw_set",
        states[state], threads, iterations, rate);
}
static void index_sample(unsigned trial, bool polling, bool future,
    size_t threads, size_t iterations) {
    expiry_index index;
    if (future) for (size_t i = 0; i < key_count; ++i)
        require(index.set("ttl:" + std::to_string(i), 4102444800123LL),
            "benchmark index seed");
    const auto keys = names(threads);
    std::vector<std::vector<std::string>> found(threads);
    const auto rate = parallel(threads, iterations, [&](size_t t, size_t i) {
        if (!polling) { index.erase(keys[t][i % key_count]); return true; }
        return !index.due(0, 64, found[t]) && found[t].empty();
    });
    require(index.size() == (future ? key_count : 0),
        "benchmark removed unrelated TTL owners");
    row(trial, "index", polling ? "poll" : "erase",
        future ? "future" : "empty", threads, iterations, rate);
}
static void changing_sample(unsigned trial, bool churn, size_t threads,
    size_t iterations) {
    expiry_index index;
    const auto keys = names(threads);
    if (!churn) for (const auto& worker : keys) for (const auto& key : worker)
        require(index.set(key, 4102444800123LL), "benchmark refresh seed");
    const auto rate = parallel(threads, iterations, [&](size_t t, size_t i) {
        const auto& key = keys[t][i % key_count];
        const bool ok = index.set(key, 4102444800123LL + (i / key_count) % 2);
        if (churn) index.erase(key);
        return ok;
    });
    require(index.size() == (churn ? 0 : threads * key_count),
        "benchmark TTL mutation count");
    row(trial, "index", churn ? "churn" : "refresh",
        churn ? "empty" : "future", threads, iterations, rate);
}
int main(int argc, char** argv) {
    acl::acl_cpp_init();
    long long iterations = 25000, trial = 1;
    require(argc <= 3 && (argc < 2 ||
        parse_redis_integer(argv[1], iterations)) && (argc < 3 ||
        parse_redis_integer(argv[2], trial)) && iterations >= 256 &&
        iterations <= 1000000 && trial > 0 && trial <= 1000,
        "usage: expiry_path_bench [iterations:256..1000000] [trial:1..1000]");
    std::cout << "trial,engine,layer,index,threads,iterations,ops_sec\n";
    for (size_t threads : {size_t(1), size_t(4)}) {
        for (bool tbb : {false, true}) {
#ifndef HAS_TBB
            if (tbb) continue;
#endif
            for (bool paged : {false, true})
                for (unsigned order = 0; order < 3; ++order)
                    store_sample(static_cast<unsigned>(trial), tbb, paged,
                        (order + trial) % 3, threads,
                        static_cast<size_t>(iterations));
        }
        for (bool future : {false, true})
            index_sample(static_cast<unsigned>(trial), false, future, threads,
                static_cast<size_t>(iterations) * 100);
        for (bool churn : {false, true})
            changing_sample(static_cast<unsigned>(trial), churn, threads,
                static_cast<size_t>(iterations) * 10);
    }
    for (bool future : {false, true})
        index_sample(static_cast<unsigned>(trial), true, future, 1,
            static_cast<size_t>(iterations) * 10);
}
