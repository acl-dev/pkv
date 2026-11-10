#include "stdafx.h"
#include "redis_list.h"
#include "redis_blocking_pop.h"
namespace pkv {
bool redis_list::blocking(const char* command, redis_coder& result) {
    redis_blocking_pop pop(handler_, obj_);
    return pop.exec(command, result);
}
} // namespace pkv
