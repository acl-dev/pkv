//
// Created by zsx on 2023/9/24.
//

#include "stdafx.h"
#include "db_watcher.h"

namespace pkv {

bool db_watchers::on_set(const std::string &key, const std::string &value, bool ok) {
    for (auto& watcher : watchers_) {
        if (!watcher->on_set(key, value, ok)) {
            return false;
        }
    }
    return true;
}

bool db_watchers::on_get(const std::string &key, const std::string &value, bool ok) {
    for (auto& watcher : watchers_) {
        if (!watcher->on_get(key, value, ok)) {
            return false;
        }
    }
    return true;
}

bool db_watchers::on_del(const std::string &key, bool ok) {
    for (auto& watcher : watchers_) {
        if (!watcher->on_del(key, ok)) {
            return false;
        }
    }
    return true;
}

void db_watchers::add_watcher(db_watcher* watcher) {
    lock_.lock();
    watchers_.emplace_back(watcher);
    lock_.unlock();
}

} // namespace pkv
