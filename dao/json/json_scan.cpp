#include "stdafx.h"
#include "json_scan.h"
#include "dao/map_status.h"
#include "db/zset_store.h"
#include "coder/compact_zset.h"
#include "coder/compact_set.h"
#include "coder/compact_hash.h"
#include "common/redis_double.h"

namespace pkv {
namespace dao {
namespace {
std::string type_name(value_kind kind) {
    switch (kind) {
    case value_kind::string: return "string";
    case value_kind::hash: return "hash";
    case value_kind::list: return "list";
    case value_kind::set: return "set";
    case value_kind::zset: return "zset";
    case value_kind::stream: return "stream";
    }
    return "none";
}
}
bool json_scan::keys(shared_db& db, const scan_options& options, scan_page& out) {
    reset(); database_.reset(); key_.clear();
    out = scan_page();
    std::vector<std::string> candidates;
    unsigned long long next;
    const auto status = db->scan_keys(options.cursor, options.count, candidates, next);
    if (status != db::scan_status::ok) {
        read_status_ = status == db::scan_status::unsupported ? read_status::unsupported_scan : read_status::storage_error;
        return false;
    }
    std::vector<std::string> matches;
    for (const auto& key : candidates) {
        if (!scan_accept(key, options)) continue;
        record_header header;
        if (!read_header(db, key, header)) {
            if (read_status_ == read_status::missing) continue;
            return false;
        }
        if (!options.has_type || type_name(header.kind) == options.type)
            matches.push_back(key);
    }
    out.cursor = next;
    out.values.swap(matches);
    return true;
}
bool json_scan::open_collection(shared_db& db, const std::string& key,
    value_kind kind) {
    database_.reset(); key_.clear();
    if (!read(db, key)) return false;
    if (view_->header().kind != kind) {
        read_status_ = read_status::wrong_type; return false;
    }
    database_ = db; key_ = key; kind_ = kind;
    return true;
}
bool json_scan::collection(const scan_options& options, scan_page& out) {
    out = scan_page();
    if (view_ && read_status_ == read_status::ok && view_->paged_payload())
        return collection_paged(options, out);
    bool ok = false;
    if (view_ && read_status_ == read_status::ok) {
        switch (view_->header().kind) {
        case value_kind::hash: ok = view_->hash_scan(options, out); break;
        case value_kind::set: ok = view_->set_scan(options, out); break;
        case value_kind::zset: ok = view_->zset_scan(options, out); break;
        default: break;
        }
    }
    if (!ok) { out = scan_page(); read_status_ = read_status::invalid_record; }
    return ok;
}
bool json_scan::collection_paged(const scan_options& options, scan_page& out) {
    if (!database_) {
        read_status_ = read_status::invalid_record; return false;
    }
    const auto result = database_->transact_page(key_,
        [&](page_transaction& tx) {
        auto& root = *tx.roots().find(key_);
        if (!root.found) return true;
        if (kind_ == value_kind::zset && compact_zset::matches(root.value)) {
            try {
                const compact_zset::view packed(root.value);
                packed.validate_fast();
                const auto deadline = packed.deadline();
                if (deadline >= 0 && deadline <= unix_time_ms()) {
                    root.action = db::update_action::erase;
                    return true;
                }
                // Small sorted sets complete the scan in one reply.
                out.values.reserve(packed.count() * 2);
                for (unsigned i = 0; i < packed.count(); ++i) {
                    const auto item = packed.ordered_at(i);
                    auto member = item.first.str();
                    if (!scan_accept(member, options)) continue;
                    out.values.push_back(std::move(member));
                    out.values.push_back(format_redis_double(packed.score(item.second)));
                }
                return true;
            } catch (const std::runtime_error&) {
                read_status_ = read_status::invalid_record;
                return false;
            }
        }
        if (kind_ == value_kind::set && compact_set::matches(root.value)) {
            try {
                const compact_set::view packed(root.value);
                const auto deadline = packed.deadline();
                if (deadline >= 0 && deadline <= unix_time_ms()) {
                    root.action = db::update_action::erase;
                    return true;
                }
                out.values.reserve(packed.count());
                for (unsigned i = 0; i < packed.count(); ++i) {
                    auto member = packed.at(i).str();
                    if (scan_accept(member, options))
                        out.values.push_back(std::move(member));
                }
                return true;
            } catch (const std::runtime_error&) {
                read_status_ = read_status::invalid_record;
                return false;
            }
        }
        if (kind_ == value_kind::hash && compact_hash::matches(root.value)) {
            try {
                const compact_hash::view packed(root.value);
                const auto deadline = packed.deadline();
                if (deadline >= 0 && deadline <= unix_time_ms()) {
                    root.action = db::update_action::erase;
                    return true;
                }
                out.values.reserve(packed.count() * (options.no_values ? 1 : 2));
                for (unsigned i = 0; i < packed.count(); ++i) {
                    const auto field = packed.at(i);
                    auto name = field.first.str();
                    if (!scan_accept(name, options)) continue;
                    out.values.push_back(std::move(name));
                    if (!options.no_values) out.values.push_back(field.second.str());
                }
                return true;
            } catch (const std::runtime_error&) {
                read_status_ = read_status::invalid_record;
                return false;
            }
        }
        if (!decode(database_, root.value)) {
            if (read_status_ != read_status::missing) return false;
            root.action = db::update_action::erase;
            return true;
        }
        if (view_->header().kind != kind_) {
            read_status_ = read_status::wrong_type; return false;
        }
        if (!view_->paged_payload()) {
            const bool ok = kind_ == value_kind::hash ?
                view_->hash_scan(options, out) : kind_ == value_kind::set ?
                view_->set_scan(options, out) : view_->zset_scan(options, out);
            if (!ok) read_status_ = read_status::invalid_record;
            return ok;
        }
        if (kind_ == value_kind::zset) {
            zset_store sorted(tx, key_);
            if (!sorted.open() || !sorted.scan(options, out) ||
                !sorted.finish()) {
                read_status_ = map_read_status(sorted.error()); return false;
            }
            return true;
        }
        if (kind_ != value_kind::hash && kind_ != value_kind::set) {
            read_status_ = read_status::unsupported_operation; return false;
        }
        map_store map(tx, key_, kind_ == value_kind::hash ? map_kind::hash :
            map_kind::set);
        if (!map.open() || !map.scan(options, out) || !map.finish()) {
            read_status_ = map_read_status(map.error()); return false;
        }
        return true;
    });
    if (result == db::batch_status::error)
        read_status_ = read_status::storage_error;
    else if (result == db::batch_status::unsupported)
        read_status_ = read_status::unsupported_operation;
    if (result != db::batch_status::ok) { out = scan_page(); return false; }
    read_status_ = read_status::ok;
    return true;
}

}
}
