//
// Created by zsx on 2023/8/19.
//

#pragma once

namespace pkv {

class test {
public:
    test() = default;

    ~test() = default;

    bool run(int argc, char *argv[]);

    static void usage(const char *procname);

    static bool test_redis_coder(const char *file);

    static void test_coder_benchmark(const char *file, size_t max);
};

} // namespace pkv