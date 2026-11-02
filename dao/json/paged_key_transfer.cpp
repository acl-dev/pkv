#include "stdafx.h"
#include "json_key.h"
#include "db/list_store.h"
#include "db/zset_store.h"
#include "dao/map_status.h"
#include "dao/native_import.h"
#include "dao/string_status.h"

namespace pkv {
namespace dao {

namespace {

dao_base::read_status transfer_error(const string_store& source,
    const string_store& destination) {
    return string_read_status(source.error() != string_store::status::ok ?
        source.error() : destination.error());
}

dao_base::read_status transfer_error(const zset_store& source,
    const zset_store& destination) {
    for (const auto error : {source.error(), destination.error()})
        if (error == zset_store::status::storage)
            return dao_base::read_status::storage_error;
    return map_read_status(source.error() != zset_store::status::ok ?
        source.error() : destination.error());
}

dao_base::read_status transfer_error(const list_store& source,
    const list_store& destination) {
    for (const auto error : {source.error(), destination.error()}) {
        if (error == list_store::status::storage)
            return dao_base::read_status::storage_error;
        if (error == list_store::status::unsupported)
            return dao_base::read_status::unsupported_format;
        if (error == list_store::status::too_large)
            return dao_base::read_status::string_too_large;
    }
    return dao_base::read_status::invalid_record;
}

dao_base::read_status transfer_error(const map_store& source,
    const map_store& destination) {
    if (source.error() != map_store::status::ok)
        return map_read_status(source.error());
    if (destination.error() != map_store::status::ok)
        return map_read_status(destination.error());
    return dao_base::read_status::invalid_record;
}

template<class Store>
dao_base::read_status clone_value(page_transaction& source_tx,
    const std::string& source, page_transaction& target_tx,
    const std::string& target, bool rename, bool& copied) {
    Store from(source_tx, source), to(target_tx, target);
    if (from.open() && from.clone_to(to, copied) &&
        (!rename || (from.clear() && from.finish())) && to.finish())
        return dao_base::read_status::ok;
    copied = false;
    return transfer_error(from, to);
}

dao_base::read_status clone_map(page_transaction& source_tx,
    const std::string& source, page_transaction& target_tx,
    const std::string& target, bool rename, bool& copied, map_kind kind) {
    map_store from(source_tx, source, kind), to(target_tx, target, kind);
    if (from.open() && from.clone_to(to, copied) &&
        (!rename || (from.clear() && from.finish())) && to.finish())
        return dao_base::read_status::ok;
    copied = false;
    return transfer_error(from, to);
}

} // namespace

bool json_key::page_result(db::batch_status status) {
    if (status == db::batch_status::error)
        read_status_ = read_status::storage_error;
    else if (status == db::batch_status::unsupported)
        read_status_ = read_status::unsupported_operation;
    if (status != db::batch_status::ok) return false;
    read_status_ = read_status::ok;
    return true;
}

bool json_key::copy_paged(shared_db& source_db, const std::string& source,
    shared_db& destination_db, const std::string& destination, bool replace,
    bool& copied, bool& retry) {
    copied = false; retry = false;
    const auto result = source_db->copy_pages_to(source, *destination_db,
        destination, [&](page_transaction& from_tx, page_transaction& to_tx) {
        auto& from = *from_tx.roots().find(source);
        const auto now = unix_time_ms();
        if (!from.found) return true;
        if (!decode(source_db, from.value, now))
            return read_status_ == read_status::missing;
        if (!view_->paged_payload()) {
            if (!view_->native_importable()) { retry = true; return false; }
            if (!view_->valid_payload()) {
                read_status_ = read_status::invalid_record; return false;
            }
        }
        auto source_view = std::move(view_);
        const auto kind = source_view->header().kind;
        auto& to = *to_tx.roots().find(destination);
        if (!replace && to.found) {
            if (decode(destination_db, to.value, now)) return true;
            if (read_status_ != read_status::missing) return false;
        }
        if (!source_view->paged_payload()) {
            if (!import_native(*source_view, to_tx, destination, read_status_))
                return false;
            copied = to.action == db::update_action::write;
            if (!copied) read_status_ = read_status::invalid_record;
            return copied;
        }
        if (kind == value_kind::string) {
            read_status_ = clone_value<string_store>(from_tx, source, to_tx,
                destination, false, copied);
            return read_status_ == read_status::ok;
        }
        if (kind == value_kind::zset) {
            read_status_ = clone_value<zset_store>(from_tx, source, to_tx,
                destination, false, copied);
            return read_status_ == read_status::ok;
        }
        read_status_ = kind == value_kind::list ? clone_value<list_store>(
            from_tx, source, to_tx, destination, false, copied) : clone_map(
                from_tx, source, to_tx, destination, false, copied,
                kind == value_kind::hash ? map_kind::hash : map_kind::set);
        return read_status_ == read_status::ok;
    });
    if (retry && result == db::batch_status::aborted) return true;
    retry = false;
    if (!page_result(result)) { copied = false; return false; }
    return true;
}

bool json_key::rename_paged(shared_db& db, const std::string& source,
    const std::string& destination, bool nx, bool& renamed, bool& retry) {
    renamed = false; retry = false;
    const auto result = db->transact_pages({source, destination},
        [&](page_transaction& tx) {
        const auto now = unix_time_ms();
        auto& from = *tx.roots().find(source);
        if (!from.found) {
            read_status_ = read_status::no_key; return false;
        }
        if (!decode(db, from.value, now)) {
            if (read_status_ == read_status::missing)
                read_status_ = read_status::no_key;
            return false;
        }
        if (!view_->paged_payload()) {
            if (!view_->native_importable()) { retry = true; return false; }
            if (!view_->valid_payload()) {
                read_status_ = read_status::invalid_record; return false;
            }
        }
        if (source == destination) { renamed = !nx; return true; }
        auto source_view = std::move(view_);
        const auto kind = source_view->header().kind;
        auto& to = *tx.roots().find(destination);
        if (nx && to.found) {
            if (decode(db, to.value, now)) return true;
            if (read_status_ != read_status::missing) return false;
        }
        if (!source_view->paged_payload()) {
            if (!import_native(*source_view, tx, destination, read_status_))
                return false;
            if (to.action != db::update_action::write) {
                read_status_ = read_status::invalid_record; return false;
            }
            from.action = db::update_action::erase;
            renamed = true; return true;
        }
        if (kind == value_kind::string) {
            read_status_ = clone_value<string_store>(tx, source, tx,
                destination, true, renamed);
            return read_status_ == read_status::ok;
        }
        if (kind == value_kind::zset) {
            read_status_ = clone_value<zset_store>(tx, source, tx, destination,
                true, renamed);
            return read_status_ == read_status::ok;
        }
        read_status_ = kind == value_kind::list ? clone_value<list_store>(
            tx, source, tx, destination, true, renamed) : clone_map(tx, source,
                tx, destination, true, renamed, kind == value_kind::hash ?
                    map_kind::hash : map_kind::set);
        return read_status_ == read_status::ok;
    });
    if (retry && result == db::batch_status::aborted) return true;
    retry = false;
    if (!page_result(result)) { renamed = false; return false; }
    return true;
}

} // namespace dao
} // namespace pkv
