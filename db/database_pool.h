#pragma once
#include "db.h"

namespace pkv {

// Shared DB instances; selecting one is connection-local. DB 0 is supplied by
// the existing startup path. Factories run serialized and publish only success.
class database_pool {
public:
    using factory = std::function<shared_db(size_t)>;
    database_pool(shared_db zero, size_t count, factory create,
        std::vector<size_t> existing = {});
    ~database_pool();
    size_t size() const;
    shared_db get(size_t index);
    std::vector<shared_db> opened();
    // Terminal: caller retains exclusive admission until the process exits.
    bool close_for_exit();
    std::vector<std::pair<size_t, shared_db>> materialized();
private:
    struct state;
    std::unique_ptr<state> state_;
};

} // namespace pkv
