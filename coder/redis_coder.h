//
// Created by shuxin ����zheng on 2023/7/19.
//

#pragma once

#include <vector>
#include "c++_patch.h"
#include "redis_object.h"

namespace pkv {

class redis_ocache;

class redis_coder {
public:
    explicit redis_coder(redis_ocache& cache);
    ~redis_coder();

    const char* update(const char* data, size_t& len);

    NODISCARD const std::vector<redis_object*>& get_objects() const {
        return objs_;
    }

    NODISCARD redis_object* get_curr() const {
        return curr_;
    }

    void clear();

    NODISCARD redis_ocache& get_cache() const {
        return cache_;
    }

public:
    NODISCARD redis_object& create_object();

    bool to_string(std::string& out) const;

private:
    redis_ocache& cache_;
    std::vector<redis_object*> objs_;
    redis_object* curr_;
};

} // namespace pkv
