//
// Created by shuxin ¡¡¡¡zheng on 2023/9/1.
//

#pragma once

namespace pkv {

class db_cursor {
public:
    db_cursor() = default;
    virtual ~db_cursor() = default;

public:
    void set_seek_key(const std::string& key);

    NODISCARD const std::string& get_seek_key() const {
        return seek_key_;
    }

    NODISCARD size_t get_db() const;
    void next_db();

    void reset();

public:
    std::string seek_key_;
    size_t dbidx_ = 0;
};

} // namespace pkv