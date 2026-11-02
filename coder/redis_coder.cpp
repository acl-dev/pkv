//
// Created by shuxin ¡¡¡¡zheng on 2023/7/19.
//

#include "stdafx.h"
#include "redis_ocache.h"
#include "redis_coder.h"

namespace pkv {

#if 0
#define EMPTY(x) ((x).size() == 0)
#else
#define EMPTY(x) ((x).empty())
#endif

#if 0
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define LIKELY(x)       (x)
#define UNLIKELY(x)     (x)
#endif

redis_coder::redis_coder(redis_ocache& cache)
: cache_(cache)
{
    curr_ = cache_.get();
}

redis_coder::~redis_coder() {
    clear();
    cache_.put(curr_);
}

void redis_coder::clear() {
    for (auto obj : objs_) {
        cache_.put(obj);
    }
    objs_.clear();
}

const char* redis_coder::update(const char* data, size_t& len) {
    while (len > 0) {
        data = curr_->update(data, len);
        if (curr_->finish()) {
            // Prepare the next owner before publishing this completed frame.
            // Otherwise a failed get() leaves curr_ owned twice.
            auto* next = cache_.get();
            try { objs_.push_back(curr_); }
            catch (...) { next->destroy(); throw; }
            curr_ = next;
        } else if (curr_->failed()) {
            break;
        }
    }

    return data;
}

redis_object& redis_coder::create_object() {
    redis_object* obj = cache_.get();
    assert(obj);
    try { objs_.push_back(obj); }
    catch (...) { obj->destroy(); throw; }
    return *obj;
}

bool redis_coder::to_string(std::string& out) const {
    for (const auto& obj : objs_) {
        if (!obj->to_resp(out)) {
            return false;
        }
    }

    return true;
}

void redis_coder::move_to(redis_object& array) {
    size_t moved = 0;
    try {
        for (; moved < objs_.size(); ++moved) array.adopt_child(*objs_[moved]);
    } catch (...) {
        objs_.erase(objs_.begin(), objs_.begin() + moved);
        throw;
    }
    objs_.clear();
}

} // namespace pkv
