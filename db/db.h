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
class db_watchers;

/**
 * @brief A class representing a key-value database.
 * 
 * This class provides methods for opening a database file, setting and getting
 * values for keys, deleting keys, creating a cursor for iterating over the keys
 * in the database, and scanning the database using a cursor. It also provides
 * methods for creating different types of databases such as rocksdb, wiredtiger,
 * memory-mapped database with hash table, AVL tree, and TBB tree.
 */
class db {
public:
    /**
     * @brief Default constructor.
     */
    db() = default;

    /**
     * @brief Virtual destructor.
     */
    virtual ~db() = default;

    /**
     * @brief Open a database file.
     * 
     * @param path The path of the database file.
     * @param watchers A pointer to a db_watchers object to be notified of database changes.
     * @return true if the database is opened successfully, false otherwise.
     */
    bool open(const char* path, db_watchers* watchers = NULL);

    /**
     * @brief Set the value of a key.
     * 
     * @param key The key.
     * @param value The value.
     * @return true if the value is set successfully, false otherwise.
     */
    bool set(const std::string& key, const std::string& value);

    /**
     * @brief Get the value of a key.
     * 
     * @param key The key.
     * @param value The value.
     * @return true if the value is retrieved successfully, false otherwise.
     */
    bool get(const std::string& key, std::string& value);

    /**
     * @brief Delete a key.
     * 
     * @param key The key.
     * @return true if the key is deleted successfully, false otherwise.
     */
    bool del(const std::string& key);

    /**
     * @brief Create a cursor for iterating over the keys in the database.
     * 
     * @return A pointer to the cursor.
     */
    db_cursor* create_cursor();

    /**
     * @brief Scan the database using a cursor.
     * 
     * @param cursor The cursor.
     * @param keys The keys to be scanned.
     * @param max The maximum number of keys to be scanned.
     * @return true if the scan is successful, false otherwise.
     */
    bool scan(db_cursor& cursor, std::vector<std::string>& keys, size_t max);

public:
    /**
     * @brief Open the database.
     * 
     * This method must be implemented by subclasses to provide database-specific implementation.
     * 
     * @param path The path of the database file.
     * @return true if the database is opened successfully, false otherwise.
     */
    virtual bool dbopen(const char* path) = 0;

    /**
     * @brief Set the value of a key.
     * 
     * This method must be implemented by subclasses to provide database-specific implementation.
     * 
     * @param key The key.
     * @param value The value.
     * @return true if the value is set successfully, false otherwise.
     */
    virtual bool dbset(const std::string& key, const std::string& value) = 0;

    /**
     * @brief Get the value of a key.
     * 
     * This method must be implemented by subclasses to provide database-specific implementation.
     * 
     * @param key The key.
     * @param value The value.
     * @return true if the value is retrieved successfully, false otherwise.
     */
    virtual bool dbget(const std::string& key, std::string& value) = 0;

    /**
     * @brief Delete a key.
     * 
     * This method must be implemented by subclasses to provide database-specific implementation.
     * 
     * @param key The key.
     * @return true if the key is deleted successfully, false otherwise.
     */
    virtual bool dbdel(const std::string& key) = 0;

    /**
     * @brief Create a cursor for iterating over the keys in the database.
     * 
     * This method must be implemented by subclasses to provide database-specific implementation.
     * 
     * @return A pointer to the cursor.
     */
    virtual db_cursor* dbcreate_cursor() = 0;

    /**
     * @brief Get the size of the database.
     *
     * This method must be implemented by subclasses to provide database-specific implementation.
     * 
     * @return The size of the database.
     */
    virtual size_t dbsize() const = 0;

    /**
     * @brief Scan the database using a cursor.
     * 
     * This method must be implemented by subclasses to provide database-specific implementation.
     * 
     * @param size The size of the database.
     * @param cursor The cursor.
     * @param keys The keys to be scanned.
     * @param max The maximum number of keys to be scanned.
     * @return true if the scan is successful, false otherwise.
     */
    virtual bool dbscan(size_t size, db_cursor& cursor,
                std::vector<std::string>& keys, size_t max) = 0;

public:
    /**
     * @brief Get the type of the database.
     * 
     * This method must be implemented by subclasses to provide database-specific implementation.
     * 
     * @return The type of the database.
     */
    NODISCARD virtual const char* get_dbtype() const = 0;

private:
    db_watchers* watchers_ = nullptr;

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
