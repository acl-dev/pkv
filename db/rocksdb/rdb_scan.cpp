#include "stdafx.h"
#ifdef HAS_ROCKSDB
#include "rdb_storage.h"
#include "db/key_count.h"
#include "rdb_cursor.h"

namespace pkv {

bool rdb::count_roots(store& storage) {
    uint64_t count = 0;
    std::unique_ptr<rocksdb::Iterator> roots(storage.data->NewIterator(
        rocksdb::ReadOptions(), storage.roots));
    for (roots->SeekToFirst(); roots->Valid(); roots->Next())
        if (!add_key_count(1, count)) return false;
    if (!roots->status().ok()) return false;
    storage.root_count.store(count, std::memory_order_relaxed);
    return true;
}
db::count_status rdb::dbkey_count(uint64_t& out) {
    uint64_t count = 0;
    for (const auto& store : stores_)
        if (!add_key_count(store->root_count.load(std::memory_order_relaxed),
            count)) return count_status::error;
    out = count; return count_status::ok;
}

bool rdb::index(store& storage) {
    std::string marker;
    auto status = storage.data->Get(rocksdb::ReadOptions(), storage.pages,
        "", &marker);
    if (status.ok()) return marker == "scan-v1";
    if (!status.IsNotFound()) return false;
    rocksdb::WriteBatch batch;
    const auto flush = [&] {
        if (!batch.Count()) return true;
        const auto result = storage.data->Write(rocksdb::WriteOptions(),
            &batch);
        batch.Clear();
        return result.ok();
    };
    // A crash before the ready marker retries a bounded rebuild on next open.
    // Only the private index is rebuilt; legacy default-CF keys stay untouched.
    std::unique_ptr<rocksdb::Iterator> old(storage.data->NewIterator(
        rocksdb::ReadOptions(), storage.scan));
    for (old->SeekToFirst(); old->Valid(); old->Next()) {
        if (!batch.Delete(storage.scan, old->key()).ok()) return false;
        if (batch.Count() >= 1024 && !flush()) return false;
    }
    if (!old->status().ok() || !flush()) return false;
    old.reset();
    std::unique_ptr<rocksdb::Iterator> roots(storage.data->NewIterator(
        rocksdb::ReadOptions(), storage.roots));
    for (roots->SeekToFirst(); roots->Valid(); roots->Next()) {
        if (!batch.Put(storage.scan, rdb_address::index(
            roots->key().ToString()), "").ok()) return false;
        if (batch.Count() >= 1024 && !flush()) return false;
    }
    if (!roots->status().ok() || !flush()) return false;
    rocksdb::WriteOptions ready;
    ready.sync = true;
    return storage.data->Put(ready, storage.pages, "", "scan-v1").ok();
}

db::scan_status rdb::dbscan_keys(unsigned long long cursor, size_t count,
    std::vector<std::string>& keys, unsigned long long& next) {
    keys.clear(); next = 0;
    if (cursor / 256 >= stores_.size()) return scan_status::ok;
    if (!count) count = 1;
    try {
        const auto limit =
            static_cast<unsigned long long>(stores_.size()) * 256;
        while (cursor < limit) {
            auto& storage = *stores_[static_cast<size_t>(cursor / 256)];
            const auto bucket = static_cast<unsigned char>(cursor % 256);
            const std::string prefix(1, static_cast<char>(bucket));
            std::unique_ptr<rocksdb::Iterator> reader(storage.data->NewIterator(
                rocksdb::ReadOptions(), storage.scan));
            for (reader->Seek(prefix); reader->Valid(); reader->Next()) {
                const auto key = reader->key();
                if (key.empty() || static_cast<unsigned char>(key[0]) != bucket)
                    break;
                keys.emplace_back(key.data() + 1, key.size() - 1);
            }
            if (!reader->status().ok()) {
                keys.clear(); return scan_status::error;
            }
            ++cursor;
            if (keys.size() >= count) break;
        }
        next = cursor < limit ? cursor : 0;
        return scan_status::ok;
    } catch (const std::bad_alloc&) {
        keys.clear(); return scan_status::error;
    }
}

db_cursor* rdb::dbcreate_cursor() { return new rdb_cursor; }

bool rdb::dbscan(size_t idx, db_cursor& cur, std::vector<std::string>& keys,
    size_t max) {
    if (idx >= stores_.size()) return false;
    auto& cursor = static_cast<rdb_cursor&>(cur);
    auto& storage = *stores_[idx];
    std::unique_ptr<rocksdb::Iterator> reader(storage.data->NewIterator(
        rocksdb::ReadOptions(), storage.roots));
    if (!cursor.started()) reader->SeekToFirst();
    else {
        reader->Seek(cursor.get_seek_key());
        if (reader->Valid() && reader->key() == cursor.get_seek_key())
            reader->Next();
    }
    for (size_t i = 0; i < max && reader->Valid(); ++i, reader->Next()) {
        auto key = reader->key().ToString();
        cursor.set_seek_key(key); keys.push_back(std::move(key));
    }
    return reader->status().ok();
}

} // namespace pkv
#endif
