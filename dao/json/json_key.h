#pragma once
#include "db/db.h"
#include "../key.h"

namespace pkv {
namespace dao {

class json_key : public key {
public:
    json_key() = default;
    ~json_key() override = default;

    bool copy(shared_db& source_db, const std::string& source, shared_db& destination_db, const std::string& destination, bool replace, bool& copied) override;
    bool rename(shared_db& db, const std::string& source, const std::string& destination, bool nx, bool& renamed) override;

    bool set_expiry(shared_db& db, const std::string& key, long long deadline_ms, unsigned conditions) override;
    bool pttl(shared_db& db, const std::string& key, long long& ms) override;
    bool expiry_time(shared_db& db, const std::string& key, long long& ms) override;
    bool persist(shared_db& db, const std::string& key) override;

    // @override
    bool del(shared_db& db, const std::string& json_key) override;

    bool del(shared_db& db, const std::vector<std::string>& keys, long long& removed) override;
    bool exists(shared_db& db, const std::vector<std::string>& keys, long long& count) override;

    // @override
    bool type(shared_db& db, const std::string& json_key, std::string& out) override;

    // @override
    bool expire(shared_db& db, const std::string& json_key, long long n) override;

    // @override
    bool ttl(shared_db& db, const std::string& json_key, long long& n) override;

    // @override
    bool scan(shared_db& db, db_cursor& cursor,
	      std::vector<std::string>& json_keys, size_t max) override;
private:
    bool copy_paged(shared_db& source_db, const std::string& source,
        shared_db& destination_db, const std::string& destination,
        bool replace, bool& copied, bool& retry);
    bool rename_paged(shared_db& db, const std::string& source,
        const std::string& destination, bool nx, bool& renamed, bool& retry);
    bool page_result(db::batch_status status);
    bool update_expiry(shared_db& db, const std::string& key,
        long long deadline, unsigned conditions, bool persist,
        bool native = true);
    bool migrate_expiry(shared_db& db, const std::string& key,
        long long deadline, unsigned conditions, bool persist, bool& changed,
        bool& unsupported);
    bool encode_expiry(shared_db& db, std::string& bytes,
        long long deadline, bool& preserve_pages);
};

} // namespace dao
} // namespace pkv
