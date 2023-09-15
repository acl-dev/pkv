//
// Created by shuxin zheng on 2023/7/19.
//

#include "stdafx.h"
#include "dao/json/json_string.h"
#include "coder/redis_ocache.h"
#include "coder/redis_coder.h"

#include "test_coder.h"

namespace pkv {

bool test_redis_parse(const char* filepath) {
    if (!test_redis_parse_once(filepath)) {
        return false;
    }

    printf("\r\n");

    if (!test_redis_parse_stream(filepath)) {
        return false;
    }

    return true;
}

bool test_redis_parse_once(const char* filepath) {
    acl::string buf;
    if (!acl::ifstream::load(filepath, buf)) {
        printf("load %s error %s\r\n", filepath, acl::last_serror());
        return false;
    }

    redis_ocache cache;
    redis_coder parser(cache);
    const char* data = buf.c_str();
    size_t len = buf.size();
    const char* left = parser.update(data, len);

    if (len > 0) {
        printf(">>>%s: parse failed<<<\r\n", __func__);
        printf("%s\r\n", left);
        return false;
    }

    printf(">>>%s: parse success<<<\r\n", __func__);

    std::string out;

    if (!parser.to_string(out)) {
        printf(">>>%s: build failed<<<\r\n", __func__);
        return false;
    }

    if (out != std::string(buf.c_str())) {
        printf(">>>%s: build failed<<<\r\n", __func__);
        printf("output:\r\n|%s|\r\n", out.c_str());
        printf("input:\r\n|%s|\r\n", buf.c_str());

        acl::string filetmp(filepath);
        filetmp += ".tmp";
        acl::ofstream fp;
        if (fp.open_trunc(filetmp)) {
            fp.write(out.c_str(), out.size());
        }

        return false;
    }

    printf("%s\r\n", out.c_str());
    printf(">>>%s: build successfully<<<\r\n", __func__);
    return true;
}

bool test_redis_parse_stream(const char* filepath) {
    acl::string buf;

    if (!acl::ifstream::load(filepath, buf)) {
        printf("load %s error %s\r\n", filepath, acl::last_serror());
        return false;
    }

    redis_ocache cache;
    redis_coder parser(cache);
    const char* data = buf.c_str();
    size_t len = buf.size();
    for (size_t i = 0; i < len; i++) {
        char ch  = *data++;
        size_t n = 1;
        //putchar(ch);
        const char* left = parser.update(&ch, n);
        if (n > 0) {
            printf(">>>%s(%d): parse failed, left=%s<<<\r\n", __func__, __LINE__, left);
            return false;
        }
    }
    printf(">>%s(%d): parse successfully<<<\r\n", __func__, __LINE__);

    std::string out;

    if (!parser.to_string(out)) {
        printf(">>%s(%d): build failed<<\r\n", __func__, __LINE__);
        return false;
    }

    if (out != std::string(buf.c_str())) {
        printf("%s\r\n", out.c_str());
        printf(">>%s(%d): build failed<<\r\n", __func__, __LINE__);
        return false;
    }

    printf("%s\r\n", out.c_str());
    const char* cmd = parser.get_objects()[0]->get_cmd();
    printf(">>%s(%d): build successfully, cmd=%s<<\r\n",
           __func__, __LINE__, cmd ? cmd : "unknown");
    return true;
}

//////////////////////////////////////////////////////////////////////////////

bool test_redis_build() {
    redis_ocache cache;
    redis_coder builder(cache);

    auto& obj = builder.create_object();

#if 0
    obj.create_child().set_string("HMSET");
    obj.create_child().set_string("hash-key");
    obj.create_child().set_string("field1");
    obj.create_child().set_string("vaule1");
    obj.create_child().set_string("field2");
    obj.create_child().set_string("value2");
    obj.create_child().set_string("field3");
    obj.create_child().set_string("value3");
#else
    obj.create_child().set_string("HMSET", true)
        .create_child().set_string("hash-key", true)
        .create_child().set_string("field1", true)
        .create_child().set_string("value1", true)
        .create_child().set_string("field2", true)
        .create_child().set_string("value2", true)
        .create_child().set_string("field3", true)
        .create_child().set_string("value3", true);
#endif

    std::string buf;
    if (builder.to_string(buf)) {
        const char* cmd = obj.get_cmd();
        printf("%s(%d): build redis successfully, cmd=%s\r\n",
               __func__, __LINE__, cmd ? cmd : "unknown");
        printf("[%s]\r\n", buf.c_str());
    } else {
        printf("%s(%d): build redis failed\r\n", __func__, __LINE__);
        return false;
    }

    return true;
}

void redis_build_bench(size_t max) {

    redis_ocache cache;
    redis_coder builder(cache);
    size_t i = 0;

    struct timeval begin, end;
    printf(">>>Begin building redis benchmark\r\n");
    gettimeofday(&begin, NULL);

    for (; i < max; i++) {
        std::string buff;

#if 1
        builder.create_object()
            .create_child().set_string("string", true)
            .create_child().set_number(-1, true)
            .create_child().set_string("hello world");
#else
        //builder.create_object().set_status("hello world!");
        //builder.create_object().set_number(-1);
        builder.create_object().set_string("hello world");
#endif

        builder.to_string(buff);
        if (i == 0) {
            printf("%s\r\n", buff.c_str());
        }
        builder.clear();
    }

    gettimeofday(&end, NULL);
    double cost = acl::stamp_sub(end, begin);
    double speed = (max * 1000) / (cost > 0 ? cost : 0.00001);
    printf(">>>Redis builder, count=%zd, cost=%.2f, speed=%.2f\r\n", max, cost, speed);

    printf("\r\n");
    printf(">>>Begin building yyjson benchmark\r\n");

    gettimeofday(&begin, NULL);

    dao::json_string dao;
    for (i = 0; i < max; i++) {
        std::string buff;
        dao.set_string("hello world");
        if (!dao.to_string(buff)) {
            printf("to_string error\r\n");
            break;
        }
        if (i == 0) {
            printf("%s\r\n", buff.c_str());
        }
        dao.reset();
    }

    gettimeofday(&end, NULL);
    cost = acl::stamp_sub(end, begin);
    speed = (max * 1000) / (cost > 0 ? cost : 0.00001);
    printf(">>>yyjson builder, count=%zd, cost=%.2f, speed=%.2f\r\n", max, cost, speed);
    printf("\r\n");
}

void redis_parse_bench(const char* filepath, size_t max) {
    acl::string buff;
    if (!acl::ifstream::load(filepath, buff)) {
        printf("Load from %s error %s\r\n", filepath, acl::last_serror());
        return;
    }

    printf(">>>Begin to parse benchmark\r\n");
    printf("Load ok:\r\n%s\r\n", buff.c_str());

    redis_ocache cache;
    redis_coder parser(cache);

    struct timeval begin, end;
    gettimeofday(&begin, NULL);

    size_t i = 0;

    for (; i < max; i++) {
        const char* data = buff.c_str();
        size_t len = buff.size();

        (void) parser.update(data, len);
        if (len > 0) {
            printf("parse error, left=%zd\r\n", len);
            break;
        }
        parser.clear();
    }

    gettimeofday(&end, NULL);
    double cost = acl::stamp_sub(end, begin);
    double speed = (i * 1000) / (cost > 0 ? cost : 0.00001);
    printf(">>>Parse ok, count=%zd, cost=%.2f, speed=%.2f\r\n", i, cost, speed);
}

} // namespace pkv
