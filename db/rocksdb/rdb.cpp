#include "stdafx.h"
#ifdef HAS_ROCKSDB
#include "rdb_storage.h"
#include "rdb_cursor.h"
#include "db/page_transaction.h"
#include <rocksdb/env.h>
#include <algorithm>
#include <limits>

namespace pkv {

rdb::rdb() = default;
rdb::~rdb() = default;

size_t rdb::locate(const std::string& key) const {
    return static_cast<size_t>(acl_hash_crc32(key.data(), key.size())) %
        stores_.size();
}

bool rdb::dbopen(const char* paths) {
    if (!paths || !stores_.empty()) return false;
    acl::string input(paths);
    std::vector<std::string> roots;
    for (const auto& base : input.split2(";, \t"))
        roots.emplace_back(base);
    return dbopen_paths(roots);
}

bool rdb::dbopen_paths(const std::vector<std::string>& paths) {
    if (paths.empty() || !stores_.empty()) return false;
    for (const auto& path : paths)
        if (path.empty() || path.find('\0') != path.npos) return false;
    for (const auto& path : paths)
        if (!open_one(path + "/rdb")) return false;
    return true;
}

bool rdb::open_one(const std::string& path) {
    if (stores_.size() >= std::numeric_limits<unsigned long long>::max() / 256)
        return false;
    if (acl_make_dirs(path.c_str(), 0755) == -1) return false;
    rocksdb::Options options;
    options.create_if_missing = true;
    options.create_missing_column_families = true;
    std::vector<std::string> names;
    const auto current = options.env->FileExists(path + "/CURRENT");
    if (current.ok()) {
        const auto status = rocksdb::DB::ListColumnFamilies(options, path,
            &names);
        if (!status.ok()) return false;
    } else if (current.IsNotFound()) names.push_back(
        rocksdb::kDefaultColumnFamilyName);
    else return false;
    for (const auto* name : {"pkv.pages.v1", "pkv.scan.v1"})
        if (std::find(names.begin(), names.end(), name) == names.end())
            names.emplace_back(name);
    std::vector<rocksdb::ColumnFamilyDescriptor> descriptors;
    for (const auto& name : names) descriptors.emplace_back(name,
        rocksdb::ColumnFamilyOptions(options));
    auto storage = std::unique_ptr<store>(new store);
    const auto status = rocksdb::DB::Open(options, path, descriptors,
        &storage->handles, &storage->data);
    if (!status.ok()) {
        logger_error("RocksDB open failed: %s", status.ToString().c_str());
        return false;
    }
    for (auto* handle : storage->handles) {
        if (handle->GetName() == rocksdb::kDefaultColumnFamilyName)
            storage->roots = handle;
        else if (handle->GetName() == "pkv.pages.v1") storage->pages = handle;
        else if (handle->GetName() == "pkv.scan.v1") storage->scan = handle;
    }
    if (!storage->roots || !storage->pages || !storage->scan ||
        !index(*storage) || !count_roots(*storage))
        return false;
    stores_.push_back(std::move(storage));
    return true;
}

bool rdb::dbget(const std::string& key, std::string& out) {
    return dblookup(key, out) == lookup_status::found;
}

db::lookup_status rdb::dblookup(const std::string& key, std::string& out) {
    try {
        auto& storage = *stores_[locate(key)];
        std::string bytes;
        const auto status = storage.data->Get(rocksdb::ReadOptions(),
            storage.roots, key, &bytes);
        if (status.ok()) { out.swap(bytes); return lookup_status::found; }
        return status.IsNotFound() ? lookup_status::missing :
            lookup_status::error;
    } catch (const std::bad_alloc&) { return lookup_status::error; }
}

db::lookup_status rdb::dblookup_page(const std::string& owner, uint64_t id,
    std::string& out) {
    try {
        auto& storage = *stores_[locate(owner)];
        std::string bytes;
        const auto status = storage.data->Get(rocksdb::ReadOptions(),
            storage.pages, rdb_address::page(owner, id), &bytes);
        if (status.ok()) { out.swap(bytes); return lookup_status::found; }
        return status.IsNotFound() ? lookup_status::missing :
            lookup_status::error;
    } catch (const std::bad_alloc&) { return lookup_status::error; }
}

bool rdb::dbset(const std::string& key, const std::string& value) {
    try {
        std::vector<batch_entry> roots;
        std::string previous;
        const auto status = dblookup(key, previous);
        if (status == lookup_status::error) return false;
        roots.emplace_back(key, status == lookup_status::found, value);
        roots.back().action = update_action::write;
        return commit(roots, {}, false) == batch_status::ok;
    } catch (const std::bad_alloc&) { return false; }
}

bool rdb::dbdel(const std::string& key) {
    try {
        std::vector<batch_entry> roots;
        std::string previous;
        if (dblookup(key, previous) != lookup_status::found) return false;
        roots.emplace_back(key, true, "");
        roots.back().action = update_action::erase;
        return commit(roots, {}, false) == batch_status::ok;
    } catch (const std::bad_alloc&) { return false; }
}

} // namespace pkv
#endif
