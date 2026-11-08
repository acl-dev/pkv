//
// Created by shuxin ¡¡¡¡zheng on 2023/7/19.
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
    redis_input_memory memory() const;

    NODISCARD redis_ocache& get_cache() const {
        return cache_;
    }

public:
    NODISCARD redis_object& create_object();

    // Own a cached reply without publishing it during a read or callback.
    class pending_reply {
    public:
        explicit pending_reply(redis_coder& owner);
        ~pending_reply();
        pending_reply(const pending_reply&) = delete;
        pending_reply& operator=(const pending_reply&) = delete;
        std::string& string();
        redis_object& object();
        void publish();
    private:
        redis_coder& owner_;
        redis_object* object_;
    };

    bool to_string(std::string& out) const;
    // Serialize replies, consuming a lone pre-encoded frame when possible.
    bool take_frame(std::string& out);
    void move_to(redis_object& array);

private:
    redis_ocache& cache_;
    std::vector<redis_object*> objs_;
    redis_object* curr_;
};

} // namespace pkv
