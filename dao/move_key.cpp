#include "stdafx.h"
#include "move_key.h"
#include "native_import.h"
#include "db/database_pool.h"
#include "db/family_move.h"
#include "db/family_publication.h"

namespace pkv {
namespace dao {
bool move_key::checked(db::batch_status status) {
    if (status == db::batch_status::ok) return true;
    if (status == db::batch_status::unsupported)
        read_status_ = read_status::unsupported_operation;
    else if (status != db::batch_status::aborted ||
        read_status_ == read_status::ok || read_status_ == read_status::missing)
        read_status_ = read_status::storage_error;
    return false;
}
bool move_key::inspect(shared_db& database, const std::string& key,
    bool& alive) {
    alive = false;
    return checked(database->transact_page_reads({key},
        [&](page_transaction& tx) {
        auto& root = *tx.roots().find(key);
        if (!root.found) return true;
        alive = decode(database, root.value);
        if (alive) return true;
        if (read_status_ != read_status::missing) return false;
        root.action = db::update_action::erase; return true;
    }));
}
bool move_key::normalize(shared_db& database, const std::string& key,
    bool& alive) {
    alive = false;
    return checked(database->transact_pages({key}, [&](page_transaction& tx) {
        auto& root = *tx.roots().find(key);
        if (!root.found) return true;
        if (!decode(database, root.value)) {
            if (read_status_ != read_status::missing) return false;
            root.action = db::update_action::erase; return true;
        }
        alive = true;
        if (view_->paged_payload()) return true;
        if (!view_->native_importable()) {
            if (root.value.size() <= page_transaction::max_value_bytes &&
                view_->valid_payload()) return true;
            read_status_ = read_status::unsupported_format; return false;
        }
        if (!import_native(*view_, tx, key, read_status_)) return false;
        if (root.action != db::update_action::write) {
            read_status_ = read_status::invalid_record; return false;
        }
        root.maintenance_only = true; return true;
    }));
}
move_memory::choice move_key::prepare(shared_db& source, shared_db& target,
    size_t from, size_t to, const move_memory::roots& roots,
    std::unique_ptr<family_publication>& publication,
    std::unique_ptr<family_publication>& effects, bool memory) {
    if (!roots.source_found || roots.target_found)
        return move_memory::choice::skip;
    record_header header;
    if (source->value_codec().stored_header(roots.source, header) !=
        codec_status::ok) {
        read_status_ = read_status::invalid_record;
        return move_memory::choice::error;
    }
    if (header.expires_at_ms >= 0 && header.expires_at_ms <= unix_time_ms())
        return move_memory::choice::skip;
    family_move intent;
    intent.source = static_cast<unsigned>(from);
    intent.target = static_cast<unsigned>(to); intent.key = roots.owner;
    intent.source_root = roots.source;
    intent.target_found = roots.target_found; intent.target_root = roots.target;
    if (!memory && !source->prepare_move_effects(*target, intent, effects)) {
        read_status_ = read_status::storage_error;
        return move_memory::choice::error;
    }
    if (source->prepare_move_observers(*target, intent, publication))
        return move_memory::choice::proceed;
    read_status_ = read_status::storage_error;
    return move_memory::choice::error;
}
bool move_key::run(database_pool& pool, move_journal* journal, size_t from,
    size_t to, const std::string& key, bool& moved) {
    execution_guard admission(true);
    reset(); journal_required_ = false; moved = false;
    if (from == to || from >= pool.size() || to >= pool.size()) {
        read_status_ = read_status::storage_error; return false;
    }
    auto source = pool.get(from), target = pool.get(to);
    if (!source || !target) {
        read_status_ = read_status::storage_error; return false;
    }
    bool alive;
    if (!inspect(source, key, alive)) return false;
    if (!alive) return true;
    if (!inspect(target, key, alive)) return false;
    if (alive) return true;
    const bool memory = source->supports_family_transfer(*target);
    if (!memory && !journal) {
        journal_required_ = true;
        read_status_ = read_status::unsupported_operation; return false;
    }
    if (!source->allows_page_writes() || !target->allows_page_writes()) {
        read_status_ = read_status::unsupported_operation; return false;
    }
    if (!normalize(source, key, alive)) return false;
    view_.reset();
    if (!alive) return true;
    std::unique_ptr<family_publication> publication, effects;
    move_memory::callbacks callbacks;
    callbacks.prepare = [&](const move_memory::roots& roots) {
        return prepare(source, target, from, to, roots, publication, effects,
            memory);
    };
    callbacks.complete = [&] {
        if (effects) effects->publish();
        if (publication) publication->publish();
    };
    db::batch_status status;
    if (memory)
        status = move_memory::run(pool, from, to, key, callbacks, moved);
    else {
        move_storage::callbacks disk;
        disk.prepare = callbacks.prepare; disk.complete = callbacks.complete;
        status = move_storage::run(pool, *journal, from, to, key, disk, moved);
    }
    publication.reset(); effects.reset();
    return checked(status);
}
} // namespace dao
} // namespace pkv
