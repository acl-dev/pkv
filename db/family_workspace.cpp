#include "stdafx.h"
#include "family_workspace_internal.h"
#include "database_pool.h"

namespace pkv {
family_workspace::family_workspace(database_pool& pool, size_t bytes,
    size_t records)
: admission_(true), state_(new state(pool, bytes, records)) {}
family_workspace::~family_workspace() = default;
bool family_workspace::failed() const { return state_->failed; }
size_t family_workspace::pending_bytes() const {
    return state_->finished || state_->failed ? 0 : state_->bytes;
}
size_t family_workspace::pending_records() const {
    return state_->finished || state_->failed ? 0 : state_->records;
}
db::batch_status family_workspace::reject(db::batch_status status) {
    state_->failed = true; return status;
}
bool family_workspace::charge(size_t bytes, size_t records) const {
    return bytes <= state_->byte_limit - state_->bytes &&
        records <= state_->record_limit - state_->records;
}
void family_workspace::discard() {
    state_->owners.clear(); state_->batches.clear(); state_->databases.clear();
    state_->bytes = state_->records = state_->cache_bytes = 0;
    state_->expanded_bytes = state_->actions = 0;
    state_->finished = true;
}
db::batch_status family_workspace::database(unsigned id, shared_db& out) {
    if (id >= state_->pool.size() || id >= page_batch::max_databases)
        return db::batch_status::aborted;
    auto found = state_->databases.find(id);
    if (found != state_->databases.end()) {
        out = found->second; return db::batch_status::ok;
    }
    auto value = state_->pool.get(id);
    if (!value) return db::batch_status::error;
    if (!value->dbsupports_detached_pages() ||
        !value->dbsupports_owned_prepare())
        return db::batch_status::unsupported;
    if (!value->opened_) return db::batch_status::error;
    for (const auto& existing : state_->databases)
        if (existing.second == value) return db::batch_status::unsupported;
    state_->databases.emplace(id, value);
    out = std::move(value); return db::batch_status::ok;
}
db::batch_status family_workspace::touch(unsigned id, const std::string& key,
    owner*& out) {
    out = nullptr;
    if (key.size() > page_batch::max_key_bytes)
        return db::batch_status::aborted;
    const address name(id, key);
    auto found = state_->owners.find(name);
    if (found != state_->owners.end()) {
        out = &found->second; return db::batch_status::ok;
    }
    if (state_->owners.size() >= state_->record_limit)
        return db::batch_status::error;
    shared_db source;
    const auto status = database(id, source);
    if (status != db::batch_status::ok) return status;
    std::string bytes;
    const auto loaded = source->dblookup_bounded_root(key, bytes);
    if (loaded == db::lookup_status::error ||
        bytes.size() > page_transaction::max_value_bytes)
        return db::batch_status::error;
    const auto cost = key.size() + 2 * bytes.size();
    if (cost > state_->byte_limit - state_->cache_bytes)
        return db::batch_status::error;
    auto inserted = state_->owners.emplace(std::piecewise_construct,
        std::forward_as_tuple(name), std::tuple<>()).first;
    auto& value = inserted->second;
    value.name = &inserted->first; value.database = std::move(source);
    value.original_found = value.present = loaded == db::lookup_status::found;
    value.original = std::move(bytes); value.value = value.original;
    if (value.present) value.data.origin = &value;
    state_->cache_bytes += cost;
    out = &value; return db::batch_status::ok;
}
db::lookup_status family_workspace::lookup(unsigned id,
    const std::string& key, std::string& out) {
    out.clear();
    if (state_->failed || state_->finished) return db::lookup_status::error;
    try {
        owner* raw;
        const auto status = touch(id, key, raw);
        if (status != db::batch_status::ok) {
            reject(status); return db::lookup_status::error;
        }
        const auto& value = *raw;
        if (!value.present) return db::lookup_status::missing;
        out = value.value; return db::lookup_status::found;
    } catch (...) {
        out.clear(); reject(db::batch_status::error);
        return db::lookup_status::error;
    }
}
db::lookup_status family_workspace::lookup_page(unsigned id,
    const std::string& key, uint64_t page, std::string& out) {
    out.clear();
    if (state_->failed || state_->finished) return db::lookup_status::error;
    try {
        owner* raw;
        const auto status = page ? touch(id, key, raw) :
            db::batch_status::aborted;
        if (status != db::batch_status::ok) {
            reject(status); return db::lookup_status::error;
        }
        const auto& value = *raw;
        if (!value.present) return db::lookup_status::missing;
        const auto& pages = value.data;
        auto found = pages.pages.find(page);
        if (found != pages.pages.end()) {
            if (found->second->action == db::update_action::erase)
                return db::lookup_status::missing;
            out = found->second->value; return db::lookup_status::found;
        }
        if (!pages.origin) return db::lookup_status::missing;
        const auto loaded = pages.origin->database->dblookup_page(
            pages.origin->name->second, page, out);
        if (loaded != db::lookup_status::found) out.clear();
        if (loaded == db::lookup_status::error)
            reject(db::batch_status::error);
        return loaded;
    } catch (...) {
        out.clear(); reject(db::batch_status::error);
        return db::lookup_status::error;
    }
}
} // namespace pkv
