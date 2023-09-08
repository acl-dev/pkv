//
// Created by shuxin zheng on 2023/9/1.
//

#pragma once

namespace pkv {

/**
 * @brief The db_cursor class provides an abstract interface for iterating over a database.
 */
class db_cursor {
public:
    /**
     * @brief Constructs a new db_cursor object.
     */
    db_cursor() = default;

    /**
     * @brief Destroys the db_cursor object.
     */
    virtual ~db_cursor() = default;

public:
    /**
     * @brief Returns the index of the current database.
     * @return The index of the current database.
     */
    NODISCARD size_t get_db() const;

    /**
     * @brief Advances the cursor to the next database.
     */
    void next_db();

    /**
     * @brief Resets the cursor to the beginning of the current database.
     */
    void reset();

    /**
     * @brief Clears the cursor's state.
     */
    virtual void clear() = 0;

private:
    size_t dbidx_ = 0;
};

} // namespace pkv