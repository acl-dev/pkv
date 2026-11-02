#include "list_block.h"
#include "redis_decimal.h"
#include "redis_integer.h"
#include "execution_gate.h"
#include <chrono>
#include <climits>
#include <cmath>
#include <strings.h>
namespace pkv {
long long blocking_clock_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
static bool word(const std::string& value, const char* expected) {
    return !strcasecmp(value.c_str(), expected);
}
bool list_block_plan::timeout(const std::string& value) {
    long double seconds;
    if (!parse_redis_decimal(value, seconds)) {
        error = "ERR timeout is not a float or out of range"; return false;
    }
    const auto milliseconds = std::ceil(seconds * 1000);
    if (milliseconds > static_cast<long double>(
        LLONG_MAX - execution_time_ms())) {
        error = "ERR timeout is out of range"; return false;
    }
    if (milliseconds < 0) { error = "ERR timeout is negative"; return false; }
    timeout_ms = static_cast<long long>(milliseconds);
    if (timeout_ms > LLONG_MAX - execution_time_ms()) {
        error = "ERR timeout is out of range"; return false;
    }
    return true;
}
bool list_block_plan::multiple(
    const std::vector<const std::string*>& args) {
    long long number;
    if (!parse_redis_integer(*args[2], number) || number < 1) {
        error = "ERR numkeys should be greater than 0"; return false;
    }
    if (static_cast<unsigned long long>(number) > args.size() - 4) {
        error = "ERR syntax error"; return false;
    }
    const auto direction = static_cast<size_t>(number) + 3;
    left = word(*args[direction], sorted ? "MIN" : "LEFT");
    if (!left && !word(*args[direction], sorted ? "MAX" : "RIGHT")) {
        error = "ERR syntax error"; return false;
    }
    if (args.size() > direction + 1) {
        if (args.size() != direction + 3 ||
            !word(*args[direction + 1], "COUNT")) {
            error = "ERR syntax error"; return false;
        }
        if (!parse_redis_integer(*args[direction + 2], count) || count < 1) {
            error = "ERR count should be greater than 0"; return false;
        }
    }
    for (size_t i = 3; i < direction; ++i) keys.push_back(*args[i]);
    return timeout(*args[1]);
}
bool list_block_plan::parse(const char* command,
    const std::vector<const std::string*>& args) {
    sorted = !strncasecmp(command, "BZ", 2);
    move = !strcasecmp(command, "BLMOVE") ||
        !strcasecmp(command, "BRPOPLPUSH");
    nested = !strcasecmp(command, "BLMPOP") ||
        !strcasecmp(command, "BZMPOP");
    if (args.size() < (nested ? 5U : move ? 4U : 3U)) {
        error = "ERR syntax error"; return false;
    }
    if (nested) return multiple(args);
    if (move) {
        left = false;
        if (!strcasecmp(command, "BLMOVE")) {
            if (args.size() != 6) { error = "ERR syntax error"; return false; }
            left = word(*args[3], "LEFT");
            to_left = word(*args[4], "LEFT");
            if ((!left && !word(*args[3], "RIGHT")) ||
                (!to_left && !word(*args[4], "RIGHT"))) {
                error = "ERR syntax error"; return false;
            }
        }
        keys.push_back(*args[1]); destination = *args[2];
    } else {
        left = !strcasecmp(command, "BLPOP") ||
            !strcasecmp(command, "BZPOPMIN");
        for (size_t i = 1; i + 1 < args.size(); ++i) keys.push_back(*args[i]);
    }
    return timeout(*args.back());
}
} // namespace pkv
