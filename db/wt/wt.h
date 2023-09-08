//
// Created by shuxin zheng on 2023/7/27.
//

#pragma once

#include "../db.h"
#include <mutex>

#ifdef HAS_WT

typedef struct __wt_connection WT_CONNECTION;

namespace pkv {

class wt_sess;

class wt {
public:
    explicit wt(size_t session_max = 10000);
    ~wt();

public:
    bool open(const char* path);

    bool set(const std::string& key, const std::string& value);

    bool get(const std::string& key, std::string& value);

    bool del(const std::string& key);

public:
    NODISCARD WT_CONNECTION* get_db() const {
        return db_;
    }

private:
    std::string path_;
    WT_CONNECTION *db_;

    std::vector<wt_sess*> sessions_;
    size_t session_max_;
    std::mutex lock_;

    wt_sess* get_session();
    void put_session(wt_sess* sess);
};

} // namespace pkv

#endif // HAS_WT
