#pragma once
#include "db.h"
#include <map>

namespace pkv {
class database_pool;
class exec_batch;
// Complete private v3 groups only. This is volatile multi-DB atomicity, not a
// durable cross-engine transaction or a source-side EXEC/MOVE policy adapter.
class replica_group {
public:
    static db::batch_status apply(database_pool& pool, const exec_batch& group);
private:
    struct plan;
    using plans = std::map<unsigned, std::unique_ptr<plan>>;
    static db::batch_status collect(database_pool& pool,
        const exec_batch& group, plans& out, std::vector<plan*>& ordered);
    static db::batch_status stage(const exec_batch& group, plans& databases);
    static void prepare(plan& item);
};
} // namespace pkv
