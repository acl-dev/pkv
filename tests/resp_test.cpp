#include "coder/redis_coder.h"
#include "coder/redis_ocache.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}

static void roundtrip(const std::string& wire, size_t chunk) {
    pkv::redis_ocache cache(2);
    pkv::redis_coder parser(cache);
    for (size_t pos = 0; pos < wire.size();) {
        size_t n = std::min(chunk, wire.size() - pos);
        const size_t supplied = n;
        parser.update(wire.data() + pos, n);
        require(n == 0 && !parser.get_curr()->failed(), "valid frame rejected");
        pos += supplied;
    }
    require(!parser.get_objects().empty(), "frame unfinished");
    std::string out;
    require(parser.to_string(out) && out == wire, "roundtrip mismatch");
}

#include "response_cases.h"

int main() {
    response_cases();
    const std::vector<std::string> valid = {
        "$0\r\n\r\n", "$-1\r\n", "*0\r\n", "*-1\r\n",
        "+\r\n", "-\r\n", ":0\r\n", ":+1\r\n",
        ":9223372036854775807\r\n", ":-9223372036854775808\r\n",
        "*3\r\n$3\r\nSET\r\n$0\r\n\r\n$0\r\n\r\n",
        "*4\r\n*0\r\n*-1\r\n$-1\r\n*1\r\n$0\r\n\r\n",
        std::string("$5\r\na\0b\r\n\r\n", 11)
    };
    std::string pipeline;
    for (const auto& wire : valid) {
        for (size_t chunk = 1; chunk <= wire.size(); ++chunk) roundtrip(wire, chunk);
        pipeline += wire;
    }
    for (size_t chunk = 1; chunk <= pipeline.size(); ++chunk) roundtrip(pipeline, chunk);
    std::string wide = "*20000\r\n";
    for (int i = 0; i < 20000; ++i) wide += "$0\r\n\r\n";
    roundtrip(wide, wide.size());
    std::vector<std::string> invalid = {
        "$-2\r\n", "*-2\r\n", "$2147483648\r\n", "$536870913\r\n",
        "*1048577\r\n", "$x\r\n", "$+1\r\n", "$ 1\r\n", "$1x\r\n",
        "$1\na\r\n", "$1\rxa\r\n", "$1\r\naX\n", "$1\r\na\rX",
        ":\r\n", ":+\r\n", ":1a\r\n", ":9223372036854775808\r\n",
        ":-9223372036854775809\r\n", "+bad\n", "+bad\rx\n",
        std::string("$1\0\r\na\r\n", 9), "+" + std::string(65537, 'x')
    };
    std::string deep;
    for (int i = 0; i < 140; ++i) deep += "*1\r\n";
    invalid.push_back(deep + "$0\r\n\r\n");
    for (const auto& wire : invalid) {
        for (size_t chunk : {size_t(1), wire.size()}) {
            pkv::redis_ocache cache(2);
            pkv::redis_coder parser(cache);
            for (size_t pos = 0; pos < wire.size() && !parser.get_curr()->failed();) {
                size_t n = std::min(chunk, wire.size() - pos);
                size_t supplied = n;
                parser.update(wire.data() + pos, n);
                pos += supplied;
            }
            require(parser.get_curr()->failed(), "malformed frame accepted");
        }
    }
    for (size_t cap : {size_t(0), size_t(1), size_t(100)}) {
        pkv::redis_ocache cache(cap);
        for (size_t end = 1; end < pipeline.size(); ++end) {
            pkv::redis_coder parser(cache);
            size_t n = end;
            parser.update(pipeline.data(), n);
        }
    }
    // Completed replies can be cleared while the next frame is incomplete.
    {
        pkv::redis_ocache cache(1);
        pkv::redis_coder parser(cache);
        const std::string first = "+OK\r\n*2\r\n$0\r\n\r\n$3\r\na";
        size_t n = first.size();
        parser.update(first.data(), n);
        require(n == 0 && parser.get_objects().size() == 1, "partial pipeline failed");
        parser.clear();
        const std::string rest = "bc\r\n";
        n = rest.size();
        parser.update(rest.data(), n);
        std::string out;
        require(n == 0 && parser.to_string(out) && out == "*2\r\n$0\r\n\r\n$3\r\nabc\r\n",
                "clear lost pending frame");
        const auto* array = parser.get_objects()[0];
        require((*array)[0] && (*array)[0][0] == '\0', "empty argument lost its type");
    }
    pkv::redis_ocache cache;
    pkv::redis_coder builder(cache);
    auto& obj = builder.create_object();
    obj.set_string("stale");
    obj.set_string("");
    builder.create_object().set_null();
    builder.create_object().set_type(pkv::REDIS_OBJ_ARRAY);
    std::string out;
    require(builder.to_string(out) && out == "$0\r\n\r\n$-1\r\n*0\r\n", "builder empty/null mismatch");
    std::cout << "RESP regression tests passed\n";
}
