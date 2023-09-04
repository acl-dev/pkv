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
    NODISCARD size_t get_db() const;
    void next_db();

    void reset();
    virtual void clear() = 0;

private:
    size_t dbidx_ = 0;
};

} // namespace pkv