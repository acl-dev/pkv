#pragma once
#include "family_plan.h"
#include "family_move.h"
#include "rpc/page_batch.h"
#include <utility>

namespace pkv {
class database_pool;
// Receiver-only view of a COMPLETE ordered group. Never hold this admission
// while waiting for network frames. One actor owns it through destruction.
// No native state changes before commit. Any stage failure poisons the group;
// this is not a source command workspace with command-local error recovery.
class family_workspace {
public:
    explicit family_workspace(database_pool& pool,
        size_t bytes = page_batch::max_bytes,
        size_t records = page_batch::max_records);
    ~family_workspace();
    db::batch_status stage(std::shared_ptr<const page_batch> batch);
    db::batch_status stage(const family_move& move);
    db::lookup_status lookup(unsigned database, const std::string& key,
        std::string& out);
    db::lookup_status lookup_page(unsigned database, const std::string& key,
        uint64_t id, std::string& out);
    db::batch_status commit(const family_plan::observer& observe = {});
    void discard();
    bool failed() const;
    size_t pending_bytes() const;
    size_t pending_records() const;
    family_workspace(const family_workspace&) = delete;
    family_workspace& operator=(const family_workspace&) = delete;
private:
    using address = std::pair<unsigned, std::string>;
    struct owner;
    struct family;
    struct state;
    execution_guard admission_;
    std::unique_ptr<state> state_;
    db::batch_status reject(db::batch_status status);
    db::batch_status database(unsigned id, shared_db& out);
    db::batch_status touch(unsigned id, const std::string& key,
        owner*& out);
    bool charge(size_t bytes, size_t records) const;
    void record_ready(owner& value);
    db::batch_status materialize(std::vector<family_edit>& out);
};
} // namespace pkv
