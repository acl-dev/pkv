#pragma once
#include <yyjson.h>
#include "db/db.h"

namespace pkv {
namespace dao {

// This is the declaration of the dao_base class.
class dao_base {
public:
    // Constructor of the dao_base class.
    dao_base();
    // Virtual destructor of the dao_base class.
    virtual ~dao_base();

public:
    // Static method to save data to the database.
    static bool save(shared_db& db, const std::string& key, const std::string& data);
    // Method to read data from the database.
    yyjson_val* read(shared_db& db, const std::string& key);

public:
    // Method to reset the dao_base object.
    void reset();

protected:
    // Flag to indicate if the dao_base object has finished its operation.
    bool finished_;
    // Type of the dao_base object.
    std::string type_;
    // Expiration time of the dao_base object.
    time_t expire_;
    // Result of the dao_base object.
    char* result_;
    // JSON writer document of the dao_base object.
    yyjson_mut_doc* w_doc_;
    // JSON writer root value of the dao_base object.
    yyjson_mut_val* w_root_;
    // JSON reader document of the dao_base object.
    yyjson_doc* r_doc_;
    // JSON reader root value of the dao_base object.
    yyjson_val* r_root_;

    // Method to create the JSON writer document and root value.
    void create_writer();
};

} // namespace dao
} // namespace pkv
