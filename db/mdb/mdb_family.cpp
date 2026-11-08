#include "stdafx.h"
#include "mdb.h"
#include "db/page_transaction.h"

namespace pkv {
bool mdb::dbsupports_family_transfer(const db& other) const {
    return dynamic_cast<const mdb*>(&other) != nullptr;
}
db::lookup_status mdb::dblookup_bounded_root(const std::string& owner,
    std::string& out) {
    const auto position = locate(owner);
    std::lock_guard<lock_t> guard(*locks_[position.shard]);
    const auto* bucket = find_store(position, false);
    if (!bucket) return lookup_status::missing;
    const auto found = bucket->find(owner);
    if (found == bucket->end()) return lookup_status::missing;
    if (found->second.value.size() > page_transaction::max_value_bytes)
        return lookup_status::error;
    try { out = found->second.value; return lookup_status::found; }
    catch (const std::bad_alloc&) { return lookup_status::error; }
}
db::batch_status mdb::dbvisit_pages(const std::string& owner,
    const page_visitor& visit) {
    const page_map* pages = nullptr;
    {
        const auto position = locate(owner);
        std::lock_guard<lock_t> guard(*locks_[position.shard]);
        const auto* family = find_page_family(position, false);
        if (!family) return batch_status::ok;
        const auto found = family->find(owner);
        if (found == family->end()) return batch_status::ok;
        pages = &found->second;
    }
    // The caller's logical key lock pins this mapped object. Hash rehashes
    // preserve its address. Never hold a shard spinlock across callbacks.
    auto result = batch_status::ok;
    pages->visit([&](uint64_t id, const std::string& bytes) {
        if (!id || bytes.size() > page_transaction::max_value_bytes)
            result = batch_status::error;
        else if (!visit(id, bytes))
            result = batch_status::aborted;
        return result == batch_status::ok;
    });
    return result;
}
} // namespace pkv
