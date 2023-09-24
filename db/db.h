#pragma once
#include <string>

namespace pkv {

/**
 * @brief The abstract base class for key-value database.
 * 
 */
class db;
using shared_db = std::shared_ptr<db>;

class db_cursor;

class db {
public:
    db() = default;
    virtual ~db() = default;

    /**
     * @brief Open a database file.
     * 
     * @param path The path of the database file.
     * @return true if the database is opened successfully, false otherwise.
     */
    virtual bool open(const char* path) = 0;

    /**
     * @brief Set the value of a key.
     * 
     * @param key The key.
     * @param value The value.
     * @return true if the value is set successfully, false otherwise.
     */
    virtual bool set(const std::string& key, const std::string& value) = 0;

    /**
     * @brief Get the value of a key.
     * 
     * @param key The key.
     * @param value The value.
     * @return true if the value is retrieved successfully, false otherwise.
     */
    virtual bool get(const std::string& key, std::string& value) = 0;

    /**
     * @brief Delete a key.
     * 
     * @param key The key.
     * @return true if the key is deleted successfully, false otherwise.
     */
    virtual bool del(const std::string& key) = 0;

    /**
     * @brief Create a cursor for iterating over the keys in the database.
     * 
     * @return A pointer to the cursor.
     */
    virtual db_cursor* create_cursor() = 0;

    /**
     * @brief Scan the database using a cursor.
     * 
     * @param cursor The cursor.
     * @param keys The keys to be scanned.
     * @param max The maximum number of keys to be scanned.
     * @return true if the scan is successful, false otherwise.
     */
    bool scan(db_cursor& cursor, std::vector<std::string>& keys, size_t max);

protected:
    /**
     * @brief Scan the database using a cursor.
     * 
     * @param size The size of the database.
     * @param cursor The cursor.
     * @param keys The keys to be scanned.
     * @param max The maximum number of keys to be scanned.
     * @return true if the scan is successful, false otherwise.
     */
    virtual bool scan(size_t size, db_cursor& cursor,
                std::vector<std::string>& keys, size_t max) = 0;

    /**
     * @brief Get the size of the database.
     * 
     * @return The size of the database.
     */
    NODISCARD virtual size_t dbsize() const = 0;

public:
    /**
     * @brief Get the type of the database.
     * 
     * @return The type of the database.
     */
    NODISCARD virtual const char* get_dbtype() const = 0;

public:
    /**
     * @brief Create a database with rocksdb.
     * 
     * @return A shared pointer to the database.
     */
    static shared_db create_rdb();

    /**
     * @brief Create a database with wiredtiger.
     * 
     * @return A shared pointer to the database.
     */
    static shared_db create_wdb();

    /**
     * @brief Create a memory-mapped database.
     * 
     * @return A shared pointer to the memory-mapped database.
     */
    static shared_db create_mdb();

    /**
     * @brief Create a memory-mapped database with hash table.
     * 
     * @return A shared pointer to the memory-mapped database with hash table.
     */
    static shared_db create_mdb_htable();

    /**
     * @brief Create a memory-mapped database with AVL tree.
     * 
     * @return A shared pointer to the memory-mapped database with AVL tree.
     */
    static shared_db create_mdb_avl();

    /**
     * @brief Create a memory-mapped database with TBB tree.
     * 
     * @return A shared pointer to the memory-mapped database with TBB tree.
     */
    static shared_db create_mdb_tbb();
};

} // namespace pkv
