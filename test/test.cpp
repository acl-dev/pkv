//
// Created by zsx on 2023/8/19.
//

#include "stdafx.h"
#include "test_coder.h"
#include "test_db.h"
#include "test.h"

namespace pkv {

void test::usage(const char *procname) {
    printf("usage: %s test -h [help]\r\n"
           " -o oper [coder, coder_bench, db_bench]\r\n"
           " -n max\r\n"
           " -f filename\r\n"
           , procname);
}

bool test::run(int argc, char **argv) {
    int ch;
    size_t max = 10000000;
    std::string filename("./string.txt"), oper("all");
    std::string dbpath("./data/test");

    optind++; // skip the test paramter of commandline.

    while ((ch = getopt(argc, argv, "hn:f:o:")) > 0) {
        switch (ch) {
        case 'h':
            usage(argv[0]);
            return true;
        case 'n':
            max = (size_t) std::atoi(optarg);
            break;
        case 'f':
            filename = optarg;
            break;
        case 'o':
            oper = optarg;
            break;
        default:
            break;
        }
    }

    if (oper == "all") {
        if (!test_redis_coder(filename.c_str())) {
            return false;
        }

        test_coder_benchmark(filename.c_str(), max);
        pkv::test_db::bench(dbpath, max);
    } else if (oper == "coder") {
        if (!test_redis_coder(filename.c_str())) {
            return false;
        }
    } else if (oper == "coder_bench") {
        test_coder_benchmark(filename.c_str(), max);
    } else if (oper == "db_bench") {
        pkv::test_db::bench(dbpath, max);
    }

    return true;
}

bool test::test_redis_coder(const char *file) {
    printf(">>>>>>>>Begin to test redis parsing<<<<<<<<<\r\n");
    if (!pkv::test_redis_parse(file)) {
        printf(">>>>>>>Test redis parsing Error<<<<<<<\r\n");
        return false;
    }
    printf(">>>>>>>Test redis parsing successfully<<<<<<<<<\r\n");

    printf("\r\n");

    printf(">>>>>>>>>Begin to test redis building<<<<<<<<\r\n");
    if (!pkv::test_redis_build()) {
        printf(">>>>>Test redis building Error<<<<<<<<\r\n");
        return false;
    }
    printf(">>>>>>>Test redis building successfully<<<<<<<\r\n");
    return true;
}

void test::test_coder_benchmark(const char *file, size_t max) {
    pkv::redis_build_bench(max);
    pkv::redis_parse_bench(file, max);
}

} // namespace pkv