#include "coder/redis_coder.h"
#include "coder/redis_ocache.h"
#include <acl-lib/acl/lib_acl.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
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

static std::string inline_response(const std::string& line) {
    std::unique_ptr<ACL_ARGV, decltype(&acl_argv_free)> words(
        acl_argv_quote_split(line.c_str(), " \t"), &acl_argv_free);
    std::string out = "*" + std::to_string(words->argc) + "\r\n";
    for (int i = 0; i < words->argc; ++i) {
        const std::string word(words->argv[i]);
        out += "$" + std::to_string(word.size()) + "\r\n" + word + "\r\n";
    }
    return out;
}

static void inline_frame(const std::string& wire,
    const std::string& expected, size_t chunk) {
    pkv::redis_ocache cache(8);
    pkv::redis_coder parser(cache);
    for (size_t pos = 0; pos < wire.size();) {
        size_t n = std::min(chunk, wire.size() - pos);
        const size_t supplied = n;
        parser.update(wire.data() + pos, n);
        require(!n && !parser.get_curr()->failed(), "inline frame rejected");
        pos += supplied;
    }
    std::string out;
    require(parser.to_string(out) && out == expected,
        "inline tokenizer differs from ACL grammar");
}

static void inline_cases() {
    std::vector<std::string> lines = {
        "PING", "  \tPING\t  ", "", " \t  ", "SET key value",
        "SET 'key space' \"value space\"", "CMD a\\ b c\\td",
        "CMD '' \"\"", "CMD a'b c'd", "CMD 'unterminated",
        std::string("CMD ") + '\\', std::string("CMD a") + '\\',
        "CMD \"a\\\"b\"", "CMD\vvalue\fword",
        std::string("CMD a\0ignored", 13), std::string("CMD \xff\x80", 6),
        std::string(65536, 'a')
    };
    unsigned state = 710126;
    const std::string alphabet = std::string("ab09 \t'\"\\\v\f\xff", 12) + '\0';
    for (unsigned i = 0; i < 128; ++i) {
        std::string line = "CMD ";
        for (unsigned j = 0; j < i % 41; ++j) {
            state = state * 1664525U + 1013904223U;
            line += alphabet[state % alphabet.size()];
        }
        lines.push_back(std::move(line));
    }
    std::string pipeline, expected;
    for (const auto& line : lines) {
        const auto response = inline_response(line);
        for (const auto& ending : {std::string("\r\n"), std::string("\n")}) {
            const auto wire = line + ending;
            for (size_t chunk : {size_t(1), size_t(7), wire.size()})
                inline_frame(wire, response, chunk);
        }
        if (line.size() < 100) {
            pipeline += line + "\r\n";
            expected += response;
        }
    }
    const std::string bulk = "*2\r\n$4\r\nECHO\r\n$3\r\na" +
        std::string("\0b\r\n", 4);
    pipeline += bulk + "PING\r\n";
    expected += bulk + inline_response("PING");
    for (size_t chunk : {size_t(1), size_t(13), pipeline.size()})
        inline_frame(pipeline, expected, chunk);
    pkv::redis_ocache cache(8);
    pkv::redis_coder parser(cache);
    const std::string first = "PING\r\nSET key va";
    size_t n = first.size(); parser.update(first.data(), n);
    require(!n && parser.get_objects().size() == 1, "partial inline count");
    parser.clear();
    const std::string rest = "lue\r\nPING\r";
    n = rest.size(); parser.update(rest.data(), n);
    parser.clear();
    n = 1; parser.update("\n", n);
    std::string out;
    require(!n && parser.to_string(out) && out == inline_response("PING"),
        "clear lost partial inline CRLF");
    for (const auto& wire : {std::string("PING\rX"),
        std::string(65537, 'a') + "\r\n"}) {
        parser.clear(); parser.get_curr()->reset();
        n = wire.size(); parser.update(wire.data(), n);
        require(parser.get_curr()->failed(), "invalid inline frame accepted");
    }
}

static void length_headers() {
    std::vector<std::pair<std::string, std::string>> cases = {
        {"$000000001\r\nx\r\n", "$1\r\nx\r\n"},
        {"$0000000001\r\nx\r\n", "$1\r\nx\r\n"},
        {"$000000000\r\n\r\n", "$0\r\n\r\n"},
        {"*000000000\r\n", "*0\r\n"},
        {"*000000002\r\n$01\r\nx\r\n$0001\r\ny\r\n",
            "*2\r\n$1\r\nx\r\n$1\r\ny\r\n"},
        {"*-1\r\n$-1\r\n", "*-1\r\n$-1\r\n"},
        {std::string("$3\r\na\0b\r\n*0\r\n", 13),
            std::string("$3\r\na\0b\r\n*0\r\n", 13)}
    };
    for (size_t size : {size_t(0), size_t(1), size_t(23), size_t(128)}) {
        std::string payload;
        for (size_t i = 0; i < size; ++i)
            payload += static_cast<char>((i * 71) & 255);
        const auto length = std::to_string(size);
        const auto prefix = std::string("*3\r\n$4\r\nECHO\r\n$");
        const auto suffix = "\r\n" + payload +
            "\r\n$0\r\n\r\n*1\r\n$4\r\nPING\r\n";
        for (size_t zeros : {size_t(0), size_t(8), size_t(9)})
            cases.emplace_back(prefix + std::string(zeros, '0') + length +
                suffix, prefix + length + suffix);
    }
    for (const auto& item : cases) {
        // Tight allocations let ASan check the logical packet boundary,
        // including packets ending within the digits or between CR and LF.
        for (size_t split = 0; split <= item.first.size(); ++split) {
            pkv::redis_ocache cache(8);
            pkv::redis_coder parser(cache);
            size_t position = 0;
            for (size_t count : {split, item.first.size() - split}) {
                if (!count) continue;
                std::unique_ptr<char[]> packet(new char[count]);
                std::memcpy(packet.get(), item.first.data() + position,
                    count);
                size_t remaining = count;
                const auto* end = parser.update(packet.get(), remaining);
                require(!remaining && end == packet.get() + count &&
                    !parser.get_curr()->failed(), "split length rejected");
                position += count;
            }
            std::string output;
            require(parser.to_string(output) && output == item.second,
                "length normalization or pipeline mismatch");
        }
    }
    const auto long_header = "$" + std::string(65535, '0') + "1\r\nx\r\n";
    for (size_t chunk : {size_t(1), size_t(9), long_header.size()})
        inline_frame(long_header, "$1\r\nx\r\n", chunk);
    for (const auto& header : {std::string("$536870912\r\n"),
        std::string("*1048576\r\n")}) {
        for (size_t chunk : {size_t(1), header.size()}) {
            pkv::redis_ocache cache(8);
            pkv::redis_coder parser(cache);
            for (size_t position = 0; position < header.size();) {
                size_t remaining = std::min(chunk, header.size() - position);
                const auto count = remaining;
                parser.update(header.data() + position, remaining);
                require(!remaining && !parser.get_curr()->failed(),
                    "maximum legal length rejected");
                position += count;
            }
            require(parser.get_objects().empty(),
                "header published before payload completion");
        }
    }
}

int main() {
    response_cases();
    inline_cases();
    length_headers();
    const std::vector<std::string> valid = {
        "$0\r\n\r\n", "$-1\r\n", "*0\r\n", "*-1\r\n",
        "+\r\n", "-\r\n", ":0\r\n", ":+1\r\n",
        ":9223372036854775807\r\n", ":-9223372036854775808\r\n",
        ":+000000000000000000001\r\n",
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
        "$2147483647\r\n", "$0000000002147483648\r\n",
        "$" + std::string(65536, '0') + "1\r\nx\r\n",
        "*1048577\r\n", "$x\r\n", "$+1\r\n", "$ 1\r\n", "$1x\r\n",
        "$1\na\r\n", "$1\rxa\r\n", "$1\r\naX\n", "$1\r\na\rX",
        ":\r\n", ":+\r\n", ":1a\r\n", ":9223372036854775808\r\n",
        ":-9223372036854775809\r\n", "+bad\n", "+bad\rx\n",
        std::string("$1\0\r\na\r\n", 9), "+" + std::string(65537, 'x')
    };
    for (const auto& bulk : {"$0\r\n\rX", "$1\r\nxX\n", "$1\r\nx\rX",
        "$01\r\nx\rX", "$536870913\r\n"})
        invalid.push_back(std::string("*1\r\n") + bulk);
    std::string boundary;
    for (int i = 0; i < 128; ++i) boundary += "*1\r\n";
    roundtrip(boundary + "$0\r\n\r\n", boundary.size() + 6);
    roundtrip(boundary + "$0\r\n\r\n", 1);
    invalid.push_back(boundary + "*1\r\n$0\r\n\r\n");
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
    {
        pkv::redis_ocache cache(1);
        auto* reply = cache.get();
        reply->set_string(std::string(1024 * 1024, 'x'));
        cache.put(reply);
        auto* reused = cache.get();
        require(reused == reply && reused->get_buf().capacity() <= 128 * 1024,
            "RESP cache retained a large payload");
        cache.put(reused);
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
