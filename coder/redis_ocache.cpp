//
// Created by zsx on 2023/8/3.
//

#include "stdafx.h"
#include "redis_object.h"
#include "redis_ocache.h"

namespace pkv {

namespace {
constexpr size_t medium_payload_min = 4 * 1024;
constexpr size_t medium_payload_budget = 256 * 1024;
}

#if 1
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

redis_ocache::redis_ocache(size_t max) : max_(max) {
#ifdef USE_VECTOR
    // Returning objects, including recursive children, must not allocate.
    cache_.reserve(max_);
#else
    cache_ = new redis_object* [max];
    for (size_t i = 0; i < max; i++) {
        cache_[i] = NULL;
    }
    pos_ = 0;
#endif
}

redis_ocache::~redis_ocache() {
#ifdef USE_VECTOR
    while (!cache_.empty()) {
        auto o = cache_.back();
        cache_.pop_back();
        o->destroy();
    }
#else
    while (pos_ > 0) {
        cache_[--pos_]->destroy();
    }
    delete [] cache_;
#endif
}

void redis_ocache::trim_idle(size_t retained) noexcept {
    if (observed_generation_ != generation_) {
        observed_generation_ = generation_;
        idle_storage_released_ = false;
        return;
    }
#ifdef USE_VECTOR
    while (cache_.size() > retained) {
        auto* object = cache_.back();
        cache_.pop_back();
        object->destroy();
    }
    if (!idle_storage_released_)
        for (auto* object : cache_) object->release_idle_storage();
#else
    while (pos_ > retained) cache_[--pos_]->destroy();
    if (!idle_storage_released_)
        for (size_t i = 0; i < pos_; ++i) cache_[i]->release_idle_storage();
#endif
    if (!idle_storage_released_) {
        for (auto& buffer : bulk_buffers_) std::string().swap(buffer);
        retained_payload_bytes_ = 0;
    }
    idle_storage_released_ = true;
}

redis_object* redis_ocache::take_cached(redis_object* object) noexcept {
    if (retained_payload_bytes_) {
        const auto capacity = object->buf_.capacity();
        if (capacity >= medium_payload_min)
            retained_payload_bytes_ -= capacity + 1;
    }
    return object;
}

void redis_ocache::retain_payload(redis_object& object) noexcept {
    const auto capacity = object.buf_.capacity();
    if (capacity < medium_payload_min) return;
    // Bound returned objects' medium payloads across all request and reply
    // batches. Large parsed payloads use the separate pool in reset().
    const auto bytes = capacity + 1;
    if (bytes > medium_payload_budget - retained_payload_bytes_) {
        std::string().swap(object.buf_);
    } else {
        retained_payload_bytes_ += bytes;
        idle_storage_released_ = false;
    }
}

bool redis_ocache::reuse_bulk(std::string& value, size_t wanted) {
    if (wanted < bulk_min || wanted > bulk_max) return false;
    std::string* best = nullptr;
    for (auto& buffer : bulk_buffers_) {
        const auto capacity = buffer.capacity();
        if (capacity >= wanted && capacity - wanted <= wanted / 8 &&
            (!best || capacity < best->capacity())) best = &buffer;
    }
    if (!best) return false;

    best->assign(value.data(), value.size());
    value.swap(*best);
    std::string().swap(*best);
    return true;
}

void redis_ocache::recycle_bulk(std::string& value) noexcept {
    const auto size = value.size();
    if (size >= bulk_min && size <= bulk_max && value.capacity() - size <= size / 8) {
        auto* slot = &bulk_buffers_[next_bulk_buffer_];
        for (auto& buffer : bulk_buffers_) {
            if (buffer.capacity() < bulk_min) {
                slot = &buffer;
                break;
            }
        }
        if (slot->capacity() >= bulk_min)
            next_bulk_buffer_ = (next_bulk_buffer_ + 1) % bulk_buffers_.size();
        value.clear();
        value.swap(*slot);
    }
    std::string().swap(value);
}

redis_object *redis_ocache::get() {
#ifdef USE_VECTOR
    if (UNLIKELY(EMPTY(cache_))) {
        return new redis_object(*this);
    } else {
        auto obj = cache_.back();
        cache_.pop_back();
        return take_cached(obj);
    }
#else
    if (LIKELY(pos_ > 0)) {
        return take_cached(cache_[--pos_]);
    } else {
        return new redis_object(*this);
    }
#endif
}

void redis_ocache::put(redis_object* obj) {
#ifdef USE_VECTOR
    if (cache_.size() < max_) {
        obj->reset();
        if (cache_.size() < max_) {
            retain_payload(*obj);
            cache_.push_back(obj);
        } else obj->destroy();
    } else {
        obj->destroy();
    }
#else
    if (LIKELY(pos_ < max_)) {
        assert(obj);
        obj->reset();
        if (pos_ < max_) {
            retain_payload(*obj);
            cache_[pos_++] = obj;
        } else obj->destroy();
    } else {
        obj->destroy();
    }
#endif
}

} // namespace pkv
