//
// Created by zsx on 2023/9/24.
//

#pragma once

namespace pkv {

class db_watcher {
public:
    db_watcher() = default;
    virtual ~db_watcher() = default;

    virtual void on_set(const std::string& key, const std::string& value, bool ok) = 0;
    virtual void on_get(const std::string& key, const std::string& value, bool ok) = 0;
    virtual void on_del(const std::string& key, bool ok) = 0;
};

} // namespace pkv
