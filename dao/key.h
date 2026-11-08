#pragma once
#include "db/db.h"
#include "dao_base.h"

namespace pkv {

class db_cursor;

namespace dao {

/**
 * @brief The key class is a DAO (Data Access Object) that provides an interface for
 *        manipulating keys in the database.
 */
class key : public dao_base {
public:
    key() = default;
    virtual ~key() override = default;

    virtual bool copy(shared_db& source_db, const std::string& source, shared_db& destination_db, const std::string& destination, bool replace, bool& copied) = 0;
    virtual bool rename(shared_db& db, const std::string& source, const std::string& destination, bool nx, bool& renamed) = 0;

    enum expire_condition { expire_nx = 1, expire_xx = 2, expire_gt = 4, expire_lt = 8 };
    virtual bool set_expiry(shared_db& db, const std::string& key,
                            long long deadline_ms, unsigned conditions) = 0;
    virtual bool pttl(shared_db& db, const std::string& key, long long& ms) = 0;
    virtual bool expiry_time(shared_db& db, const std::string& key, long long& ms) = 0;
    virtual bool persist(shared_db& db, const std::string& key) = 0;

    /**
     * @brief Deletes a key from the database.
     * @param db The shared database instance.
     * @param key The key to delete.
     * @return True if the key was deleted successfully, false otherwise.
     */
    virtual bool del(shared_db& db, const std::string& key) = 0;
    virtual bool del_one(shared_db& db, const std::string& key, long long& removed) = 0;
    virtual bool del(shared_db& db, const std::vector<std::string>& keys, long long& removed) = 0;
    virtual bool exists(shared_db& db, const std::vector<std::string>& keys, long long& count) = 0;

    /**
     * @brief Gets the type of a key in the database.
     * @param db The shared database instance.
     * @param key The key to get the type of.
     * @param out The output string to store the type of the key.
     * @return True if the type was retrieved successfully, false otherwise.
     */
    virtual bool type(shared_db& db, const std::string& key, std::string& out) = 0;

    /**
     * @brief Sets an expiration time (in seconds) for a key in the database.
     * @param db The shared database instance.
     * @param key The key to set the expiration time for.
     * @param n The number of seconds until the key expires.
     * @return True if the expiration time was set successfully, false otherwise.
     */
    virtual bool expire(shared_db& db, const std::string& key, long long n) = 0;

    /**
     * @brief Gets the remaining time (in seconds) until a key in the database expires.
     * @param db The shared database instance.
     * @param key The key to get the remaining time for.
     * @param n The output integer to store the remaining time.
     * @return True if the remaining time was retrieved successfully, false otherwise.
     */
    virtual bool ttl(shared_db& db, const std::string& key, long long& n) = 0;

    /**
     * @brief Scans the database for keys matching a pattern.
     * @param db The shared database instance.
     * @param cursor The database cursor to use for scanning.
     * @param keys The output vector to store the matching keys.
     * @param max The maximum number of keys to return.
     * @return True if the scan was successful, false otherwise.
     */
    virtual bool scan(shared_db& db, db_cursor& cursor,
	      std::vector<std::string>& keys, size_t max) = 0;

private:
};

} // namespace dao
} // namespace pkv
