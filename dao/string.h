#pragma once
#include "db/db.h"
#include "dao_base.h"
#include "common/redis_bitmap.h"
#include "common/bitop.h"
#include "common/bitfield.h"
#include "common/lcs.h"

namespace pkv {
namespace dao {


struct set_options {
    enum condition {
        any, only_missing, only_existing, only_equal, only_different,
        digest_equal, digest_different, invalid_condition
    } when = any;
    bool return_old = false;
    bool keep_ttl = false;
    long long expires_at_ms = -1;
    // Borrowed for this synchronous call; never retained in storage/workspaces.
    const std::string* comparison = nullptr;
    bool compares() const {
        return when >= only_equal && when <= digest_different;
    }
    bool digest() const { return when == digest_equal ||
        when == digest_different; }
    bool equality() const { return when == only_equal || when == digest_equal; }
    bool needs_existing() const { return when == only_existing || equality(); }
};
// Values are borrowed for the synchronous batch call, never retained.
using string_assignments = std::map<std::string, const std::string*>;
struct set_result {
    bool applied = false;
    bool had_old = false;
    std::string old_value;
};

struct get_options {
    enum mode { unchanged, erase, persist, ex, px, exat, pxat } expiry = unchanged;
    std::string amount;
};

struct string_result {
    bool found = false;
    std::string value;
};

// This class represents a string data type in the database.
// It inherits from the dao_base class.
class string : public dao_base {
public:
    // Default constructor.
    string() = default;

    // Virtual destructor.
    virtual ~string() override = default;

    // Sets the value of a string key in the database.
    // Returns true if successful, false otherwise.
    virtual bool set(shared_db& db, const std::string& key, const char* data) = 0;

    // Byte-preserving overload used by command handlers.
    virtual bool set(shared_db& db, const std::string& key, const std::string& data) = 0;

    virtual bool set(shared_db& db, const std::string& key, const std::string& data,
                     const set_options& options, set_result& result) = 0;

    virtual bool digest(shared_db&, const std::string&, string_result& out) {
        out = string_result();
        read_status_ = read_status::unsupported_operation; return false;
    }
    virtual bool delex(shared_db&, const std::string&, const set_options&,
        long long& removed) {
        removed = 0; read_status_ = read_status::unsupported_operation;
        return false;
    }
    virtual bool mget(shared_db& db, const std::vector<std::string>& keys,
                      std::vector<string_result>& values) = 0;
    virtual bool mset(shared_db& db, const std::map<std::string, std::string>& values,
                      bool only_missing, bool& applied) = 0;

    virtual bool msetex(shared_db&, const string_assignments&,
        const set_options&, bool& applied) {
        applied = false; read_status_ = read_status::unsupported_operation;
        return false;
    }
    virtual bool increment(shared_db& db, const std::string& key, long long delta,
                           bool subtract, long long& value) = 0;
    virtual bool increment_float(shared_db&, const std::string&,
        const std::string&, std::string& out) {
        out.clear(); read_status_ = read_status::unsupported_operation;
        return false;
    }
    virtual bool getbit(shared_db&, const std::string&, long long,
        long long& out) {
        out = 0; read_status_ = read_status::unsupported_operation;
        return false;
    }
    virtual bool setbit(shared_db&, const std::string&, long long, int,
        long long& out) {
        out = 0; read_status_ = read_status::unsupported_operation;
        return false;
    }
    virtual bool bitcount(shared_db&, const std::string&, const bitmap_range&,
        long long& out) {
        out = 0; read_status_ = read_status::unsupported_operation;
        return false;
    }
    virtual bool bitpos(shared_db&, const std::string&, int,
        const bitmap_range&, long long& out) {
        out = -1; read_status_ = read_status::unsupported_operation;
        return false;
    }
    virtual bool bitop(shared_db&, bitmap_operation,
        const std::vector<std::string>&, const std::string&, long long& out) {
        out = 0; read_status_ = read_status::unsupported_operation;
        return false;
    }
    virtual bool bitfield(shared_db&, const std::string&,
        const std::vector<bitfield_operation>&, bool,
        std::vector<bitfield_reply>& out) {
        out.clear(); read_status_ = read_status::unsupported_operation;
        return false;
    }
    virtual bool lcs(shared_db&, const std::string&, const std::string&,
        const std::vector<const std::string*>&, lcs_options&, lcs_result& out,
        const char*& error) {
        out = lcs_result(); error = nullptr;
        read_status_ = read_status::unsupported_operation; return false;
    }
    virtual bool append(shared_db& db, const std::string& key, const std::string& suffix, long long& length) = 0;
    virtual bool setrange(shared_db& db, const std::string& key, long long offset,
                          const std::string& value, long long& length) = 0;
    virtual bool length(shared_db& db, const std::string& key, long long& length) = 0;
    virtual bool getrange(shared_db& db, const std::string& key, long long start, long long end, std::string& out) = 0;
    virtual bool getex(shared_db& db, const std::string& key, const get_options& options, string_result& result) = 0;

    // Gets the value of a string key from the database.
    // The value is returned in the out parameter.
    // Returns true if successful, false otherwise.
    virtual bool get(shared_db& db, const std::string& key, std::string& out) = 0;
};


} // namespace dao
} // namespace pkv
