//
// Created by zsx on 2023/9/24.
//

#include "stdafx.h"
#include "sync_watcher.h"

namespace pkv {

sync_watcher::sync_watcher() {
}

sync_watcher::~sync_watcher() {
}

void sync_watcher::on_set(const std::string &key, const std::string &value,
       bool ok) {

}

void sync_watcher::on_get(const std::string &key, const std::string &value,
       bool ok) {

}

void sync_watcher::on_del(const std::string &key, bool ok) {

}

} // namespace pkv
