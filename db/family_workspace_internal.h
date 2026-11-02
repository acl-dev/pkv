#pragma once
#include "family_workspace.h"
#include <map>
#include <tuple>

namespace pkv {
struct family_workspace::family {
    const owner* origin = nullptr;
    std::map<uint64_t, const page_change*> pages;
};
struct family_workspace::owner {
    owner() = default;
    owner(const owner&) = delete;
    owner& operator=(const owner&) = delete;
    const address* name = nullptr;
    shared_db database;
    bool original_found = false, present = false;
    bool dirty = false, logical = false, history = false;
    std::string original, value;
    family data;
    size_t list_rank = SIZE_MAX, zset_rank = SIZE_MAX;
};
struct family_workspace::state {
    database_pool& pool;
    const size_t byte_limit, record_limit;
    size_t bytes = 0, records = 0, cache_bytes = 0, rank = 0;
    bool failed = false, finished = false;
    std::map<unsigned, shared_db> databases;
    // Owning batches outlive the borrowed delta pointers in owner families.
    std::vector<std::shared_ptr<const page_batch>> batches;
    std::map<address, owner> owners;
    state(database_pool& source, size_t bytes_limit, size_t records_limit)
    : pool(source), byte_limit(std::min(bytes_limit, page_batch::max_bytes)),
      record_limit(std::min(records_limit, page_batch::max_records)) {}
};
} // namespace pkv
