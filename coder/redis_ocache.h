//
// Created by zsx on 2023/8/3.
//

#pragma once
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
    size_t max_;
    unsigned long long generation_ = 0, observed_generation_ = 0;
#ifdef USE_VECTOR
    std::vector<redis_object*> cache_;
#else
    redis_object** cache_;
    size_t pos_;
#endif
};

} // namespace pkv
