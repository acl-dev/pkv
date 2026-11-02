#include "stdafx.h"
#include "native_import.h"
#include "map_status.h"
#include "string_status.h"

namespace pkv {
namespace dao {
namespace {
using read_status = dao_base::read_status;

bool store_error(string_store& store, read_status& error) {
    error = string_read_status(store.error());
    return store.error() == string_store::status::ok;
}
bool store_error(map_store& store, read_status& error) {
    error = map_read_status(store.error());
    return store.error() == map_store::status::ok;
}
bool store_error(zset_store& store, read_status& error) {
    error = map_read_status(store.error());
    return store.error() == zset_store::status::ok;
}
bool store_error(list_store& store, read_status& error) {
    switch (store.error()) {
    case list_store::status::ok: return true;
    case list_store::status::unsupported:
        error = read_status::unsupported_format; break;
    case list_store::status::storage:
        error = read_status::storage_error; break;
    case list_store::status::too_large:
        error = read_status::string_too_large; break;
    default: error = read_status::invalid_record; break;
    }
    return false;
}

template<class Store, class Import>
bool finish_import(record_view& view, Store& store, Import import,
    read_status& error) {
    return import(view, store, error) &&
        (store.finish() || store_error(store, error));
}
} // namespace

bool import_string(record_view& view, string_store& store,
    read_status& error) {
    size_t length;
    if (!view.string_length(length)) {
        error = read_status::invalid_record; return false;
    }
    if (length > string_page_codec::max_bytes) {
        error = read_status::string_too_large; return false;
    }
    store.initialize(view.header().expires_at_ms);
    if (!store_error(store, error)) return false;
    if (length > string_page_codec::write_bytes) {
        bool invalid = false;
        const bool ok = store.replace_chunks(length,
            [&](uint64_t offset, size_t count, std::string& bytes) {
                invalid = !view.string_range(static_cast<long long>(offset),
                    static_cast<long long>(offset + count - 1), bytes);
                return !invalid;
            }) && store.set_expiry(view.header().expires_at_ms);
        if (invalid) {
            error = read_status::invalid_record; return false;
        }
        return ok || store_error(store, error);
    }
    for (size_t offset = 0; offset < length;) {
        const auto size = std::min(length - offset,
            string_page_codec::page_bytes);
        std::string bytes;
        if (!view.string_range(static_cast<long long>(offset),
            static_cast<long long>(offset + size - 1), bytes) ||
            bytes.size() != size) {
            error = read_status::invalid_record; return false;
        }
        if (!store.append(bytes)) return store_error(store, error);
        offset += size;
    }
    return true;
}

bool import_list(record_view& view, list_store& store,
    read_status& error) {
    size_t count;
    if (!view.list_size(count) || !count || count >
        static_cast<unsigned long long>(LLONG_MAX)) {
        error = read_status::invalid_record;
        return false;
    }
    store.initialize(view.header().expires_at_ms);
    // The old decoder owns the legacy document. Import a bounded batch of
    // members at a time, never an additional vector of the entire List.
    std::vector<std::string> batch;
    size_t bytes = 0;
    for (size_t i = 0; i < count; ++i) {
        std::vector<std::string> member;
        const auto position = static_cast<long long>(i);
        if (!view.list_range(position, position, member) ||
            member.size() != 1) {
            error = read_status::invalid_record;
            return false;
        }
        bytes += member.front().size();
        batch.push_back(std::move(member.front()));
        if (bytes >= list_page_codec::max_page_bytes || batch.size() >= 64) {
            if (!store.push(batch, false)) return store_error(store, error);
            batch.clear();
            bytes = 0;
        }
    }
    if (!batch.empty() && !store.push(batch, false))
        return store_error(store, error);
    return store_error(store, error);
}

bool import_hash(record_view& view, map_store& store,
    read_status& error) {
    size_t count;
    if (!view.hash_size(count) || count > static_cast<uint64_t>(LLONG_MAX)) {
        error = read_status::invalid_record;
        return false;
    }
    store.initialize(view.header().expires_at_ms);
    bool valid = store_error(store, error);
    if (!valid) return false;
    const bool visited = view.hash_visit([&](const std::string& key,
        const std::string& value) {
        bool added;
        if (!store.put(key, value, true, added)) {
            store_error(store, error); valid = false; return false;
        }
        if (!added) {
            error = read_status::invalid_record; valid = false;
            return false;
        }
        return true;
    });
    if (!visited || (valid && store.metadata().elements != count)) {
        error = read_status::invalid_record;
        return false;
    }
    return valid;
}

bool import_set(record_view& view, map_store& store,
    read_status& error) {
    size_t count;
    if (!view.set_size(count) || !count ||
        count > static_cast<uint64_t>(LLONG_MAX)) {
        error = read_status::invalid_record; return false;
    }
    store.initialize(view.header().expires_at_ms);
    bool valid = store.error() == map_store::status::ok;
    if (!valid) return store_error(store, error);
    const bool visited = view.set_visit([&](const std::string& member) {
        bool added;
        if (!store.put(member, "", true, added)) {
            error = map_read_status(store.error());
            valid = false; return false;
        }
        if (!added) {
            error = read_status::invalid_record;
            valid = false; return false;
        }
        return true;
    });
    if (!visited || (valid && store.metadata().elements != count)) {
        error = read_status::invalid_record; return false;
    }
    return valid;
}

bool import_zset(record_view& view, zset_store& store,
    read_status& error) {
    size_t count;
    if (!view.zset_size(count) || !count ||
        count > static_cast<uint64_t>(LLONG_MAX)) {
        error = read_status::invalid_record; return false;
    }
    store.initialize(view.header().expires_at_ms);
    bool valid = store.error() == zset_store::status::ok;
    if (!valid) return store_error(store, error);
    const bool visited = view.zset_visit([&](const std::string& member,
        double score) {
        bool added, changed;
        if (!store.put(member, score, added, changed)) {
            error = map_read_status(store.error());
            valid = false; return false;
        }
        if (!added) {
            error = read_status::invalid_record;
            valid = false; return false;
        }
        return true;
    });
    if (!visited || (valid && store.metadata().elements != count)) {
        error = read_status::invalid_record; return false;
    }
    return valid;
}

bool import_native(record_view& view, page_transaction& transaction,
    const std::string& key, dao_base::read_status& error) {
    switch (view.header().kind) {
    case value_kind::string: {
        string_store store(transaction, key);
        return finish_import(view, store, import_string, error);
    }
    case value_kind::list: {
        list_store store(transaction, key);
        return finish_import(view, store, import_list, error);
    }
    case value_kind::hash:
    case value_kind::set: {
        const bool hash = view.header().kind == value_kind::hash;
        map_store store(transaction, key, hash ? map_kind::hash :
            map_kind::set);
        return finish_import(view, store, hash ? import_hash : import_set,
            error);
    }
    case value_kind::zset: {
        zset_store store(transaction, key);
        return finish_import(view, store, import_zset, error);
    }
    }
    error = dao_base::read_status::invalid_record;
    return false;
}

} // namespace dao
} // namespace pkv
