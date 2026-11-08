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

redis_coder::pending_reply::pending_reply(redis_coder& owner)
    : owner_(owner), object_(owner.cache_.get()) {}

redis_coder::pending_reply::~pending_reply() {
    if (object_) owner_.cache_.put(object_);
}

std::string& redis_coder::pending_reply::string() {
    assert(object_);
    object_->set_type(REDIS_OBJ_STRING);
    return object_->buf_;
}

redis_object& redis_coder::pending_reply::object() {
    assert(object_);
    return *object_;
}

void redis_coder::pending_reply::publish() {
    assert(object_);
    owner_.objs_.push_back(object_);
    object_ = nullptr;
}

bool redis_coder::to_string(std::string& out) const {
    for (const auto& obj : objs_) {
        if (!obj->to_resp(out)) {
            return false;
        }
    }

    return true;
}

bool redis_coder::take_frame(std::string& out) {
    using kind = redis_object::response_kind;
    if (out.empty() && objs_.size() == 1) {
        auto& reply = *objs_.front();
        const auto format = reply.response_kind_;
        const auto protocol = reply.response_protocol_;
        if (reply.get_type() == REDIS_OBJ_STRING && !reply.size() &&
            (protocol == 0 || protocol == 2 || protocol == 3) &&
            (format == kind::raw || format == kind::raw_set)) {
            if (format == kind::raw_set && protocol == 3) {
                if (reply.buf_.empty() || reply.buf_.front() != '*') return false;
                reply.buf_.front() = '~';
            }
            out.swap(reply.buf_);
            return true;
        }
    }
    // Flat replies already know their wire sizes. Reserve the whole batch
    // before appending so a large pipeline does not grow the frame repeatedly.
    if (out.empty() && objs_.size() > 1 && objs_.front()->buf_.size() >= 1024) {
        size_t bytes = 0;
        for (const auto* reply : objs_) {
            const auto format = reply->response_kind_;
            if ((format != kind::raw && format != kind::raw_set) ||
                reply->buf_.size() > out.max_size() - bytes) return to_string(out);
            bytes += reply->buf_.size();
        }
        out.reserve(bytes);
    }
    return to_string(out);
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
