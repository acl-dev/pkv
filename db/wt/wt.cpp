//
// Created by shuxin ¡¡¡¡zheng on 2023/7/27.
//

#include "stdafx.h"

#ifdef HAS_WT

#include <wiredtiger.h>
#include "wt_sess.h"
#include "wt.h"

namespace pkv {

wt::wt(size_t session_max) : db_(nullptr), session_max_(session_max) {}

wt::~wt() {
    if (db_) {
        db_->close(db_, nullptr);
        logger("wdb in %s closed", path_.c_str());
    }
}

bool wt::open(const char *path) {
    path_ = path;

    //std::string conf = "create,log=(enabled),checkpoint=(wait=5),cache_size=1G";
    std::string conf = "create,log=(enabled),cache_size=1G";
    int ret = wiredtiger_open(path_.c_str(), nullptr, conf.c_str(), &db_);
    if (ret != 0) {
        logger_error("open %s error=%d", path_.c_str(), ret);
        return false;
    }

    return true;
}

bool wt::set(const std::string &key, const std::string &value) {
    auto sess = get_session();
    if (sess == nullptr) {
        return false;
    }

    bool ret = sess->add(key, value);
    put_session(sess);
    return ret;
}

bool wt::get(const std::string &key, std::string &value) {
    auto sess = get_session();
    if (sess == nullptr) {
        return false;
    }

    bool ret = sess->get(key, value);
    put_session(sess);
    return ret;
}

bool wt::del(const std::string &key) {
    auto sess = get_session();
    if (sess == nullptr) {
        return false;
    }

    bool ret = sess->del(key);
    put_session(sess);
    return ret;
}

wt_sess* wt::get_session() {
    lock_.lock();
    if (!sessions_.empty()) {
        auto sess = sessions_.back();
        sessions_.pop_back();

        lock_.unlock();
        return sess;
    }
    lock_.unlock();

    auto sess = new wt_sess(*this);
    if (sess->open()) {
        return sess;
    }

    delete sess;
    return nullptr;
}

void wt::put_session(wt_sess* sess) {
    lock_.lock();
    if (sessions_.size() < session_max_) {
        sessions_.emplace_back(sess);
        lock_.unlock();
    } else {
        lock_.unlock();
        delete sess;
    }
}

} // namespace pkv

#endif // HAS_WT
