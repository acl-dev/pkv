#pragma once
#include "db/db.h"
#include "dao_base.h"

namespace pkv {
namespace dao {


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

    // Gets the value of a string key from the database.
    // The value is returned in the out parameter.
    // Returns true if successful, false otherwise.
    virtual bool get(shared_db& db, const std::string& key, std::string& out) = 0;
};


} // namespace dao
} // namespace pkv
