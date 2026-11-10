//
// Created by zsx on 2023/8/3.
//

#pragma once
#include <array>
#include <string>
#include <vector>

namespace pkv {

class redis_object;

class redis_ocache {
public:
    redis_ocache(size_t max = 100);
    ~redis_ocache();

    redis_object* get();
    void put(redis_object* obj);

    // Worker-local: active batches and the maintenance fiber share a scheduler.
    void activity() noexcept { ++generation_; }
    // Keep live borrowers untouched; two ticks without activity reclaim only
    // objects that have already been reset and returned to this cache.
    void trim_idle(size_t retained) noexcept;


private:
    friend class redis_object;
    static constexpr size_t bulk_min = 128 * 1024;
    static constexpr size_t bulk_max = 256 * 1024;
    bool reuse_bulk(std::string& value, size_t wanted);
    void recycle_bulk(std::string& value) noexcept;
    redis_object* take_cached(redis_object* object) noexcept;
    void retain_payload(redis_object& object) noexcept;

    // Large request storage is bounded independently of the object cache.
    std::array<std::string, 4> bulk_buffers_;
    size_t next_bulk_buffer_ = 0;
    size_t max_;
    unsigned long long generation_ = 0, observed_generation_ = 0;
    bool idle_storage_released_ = false;
    size_t retained_payload_bytes_ = 0;
#ifdef USE_VECTOR
    std::vector<redis_object*> cache_;
#else
    redis_object** cache_;
    size_t pos_;
#endif
};

} // namespace pkv
