#pragma once
#include <functional>

namespace pkv {
class redis_handler;
class redis_object;
class redis_coder;
using redis_handler_t = std::function<bool(redis_handler&, const char*,
    const redis_object&, redis_coder&)>;
} // namespace pkv
