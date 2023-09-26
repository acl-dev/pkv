//
// Created by zsx on 2023/9/24.
//

#pragma once

namespace pkv {

class db_watcher {
public:
    db_watcher() = default;
    virtual ~db_watcher() = default;

    virtual bool on_set(const std::string& key, const std::string& value, bool ok) = 0;
    virtual bool on_get(const std::string& key, const std::string& value, bool ok) = 0;
    virtual bool on_del(const std::string& key, bool ok) = 0;
};

class db_watchers {
public:
    db_watchers() = default;
    ~db_watchers() = default;

    bool on_set(const std::string& key, const std::string& value, bool ok);
    bool on_get(const std::string& key, const std::string& value, bool ok);
    bool on_del(const std::string& key, bool ok);

    void add_watcher(db_watcher* watcher);

private:
    std::mutex lock_;
    std::vector<db_watcher*> watchers_;
};

} // namespace pkv
