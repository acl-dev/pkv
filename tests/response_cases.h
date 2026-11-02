#pragma once

static void response_cases() {
    using namespace pkv;
    using kind = redis_object::response_kind;
    redis_ocache cache(100);
    redis_coder replies(cache);
    auto& map = replies.create_object();
    map.set_type(REDIS_OBJ_ARRAY);
    map.response_as(kind::map);
    map.create_child().set_string("nil");
    map.create_child().set_null();
    map.create_child().set_string("real");
    map.create_child().set_string("1.25").response_as(kind::real);
    std::string legacy, modern;
    require(map.to_resp(legacy, 2) && legacy ==
        "*4\r\n$3\r\nnil\r\n$-1\r\n$4\r\nreal\r\n$4\r\n1.25\r\n",
        "RESP2 map/double fallback");
    require(map.to_resp(modern, 3) && modern ==
        "%2\r\n$3\r\nnil\r\n_\r\n$4\r\nreal\r\n,1.25\r\n",
        "RESP3 map/null/double");
    replies.clear();
    auto& pairs = replies.create_object();
    pairs.response_as(kind::pairs);
    pairs.create_child().set_string(std::string("x\0", 2));
    pairs.create_child().set_string("-inf").response_as(kind::real);
    std::string bytes;
    const char expected[] = "*1\r\n*2\r\n$2\r\nx\0\r\n,-inf\r\n";
    require(pairs.to_resp(bytes, 3) && bytes ==
        std::string(expected, sizeof(expected) - 1),
        "RESP3 pair ordering and binary length");
    replies.clear();
    auto& outer = replies.create_object();
    auto& first = outer.create_child().set_null();
    first.response_protocol(2);
    auto& second = outer.create_child().set_null();
    second.response_protocol(3);
    bytes.clear();
    require(outer.to_resp(bytes, 3) && bytes == "*2\r\n$-1\r\n_\r\n",
        "per-command protocol in an EXEC array");
    replies.clear();
    for (unsigned i = 0; i < 100; ++i) {
        auto& object = replies.create_object();
        object.set_type(REDIS_OBJ_ARRAY);
        bytes.clear();
        require(object.to_resp(bytes, 3) && bytes == "*0\r\n",
            "cached response annotation leaked");
        object.response_as(kind::set); bytes.clear();
        require(object.to_resp(bytes, 3) && bytes == "~0\r\n", "empty set");
        object.response_as(kind::map); bytes.clear();
        require(object.to_resp(bytes, 3) && bytes == "%0\r\n", "empty map");
        object.response_protocol(2); bytes.clear();
        require(object.to_resp(bytes, 3) && bytes == "*0\r\n", "override");
        replies.clear();
    }
    auto& verbatim = replies.create_object();
    verbatim.set_string(std::string("a\0b", 3)).response_as(kind::verbatim);
    bytes.clear();
    require(verbatim.to_resp(bytes, 3) &&
        bytes == std::string("=7\r\ntxt:a\0b\r\n", 13), "verbatim binary");
    bytes.clear();
    require(verbatim.to_resp(bytes, 2) &&
        bytes == std::string("$3\r\na\0b\r\n", 9), "verbatim RESP2 fallback");
    replies.clear();
    auto& execute = replies.create_object();
    execute.set_type(REDIS_OBJ_ARRAY);
    execute.create_child().set_string(
        ">3\r\n$9\r\nsubscribe\r\n$1\r\na\r\n:1\r\n"
        ">3\r\n$9\r\nsubscribe\r\n$1\r\nb\r\n:2\r\n").response_as(kind::raw);
    execute.create_child().set_status("PONG");
    bytes.clear();
    require(execute.to_resp(bytes, 3) && bytes ==
        "*2\r\n>3\r\n$9\r\nsubscribe\r\n$1\r\na\r\n:1\r\n"
        ">3\r\n$9\r\nsubscribe\r\n$1\r\nb\r\n:2\r\n+PONG\r\n",
        "multiple raw subscription acknowledgements inside EXEC");
    replies.clear();
    auto& odd = replies.create_object();
    odd.response_as(kind::map).create_child().set_string("key");
    bytes.clear();
    require(!odd.to_resp(bytes, 3) && bytes.empty(), "odd map accepted");
    require(!odd.to_resp(bytes, 4), "invalid output protocol accepted");
}
