#include "stdafx.h"
#ifdef HAS_ROCKSDB
#include "rdb_storage.h"
#include "db/key_count.h"
#include "db/page_transaction.h"

namespace pkv {

db::batch_status rdb::dbapply(const std::vector<batch_entry>& roots) {
    return commit(roots, {}, false);
}

db::batch_status rdb::dbapply_pages(const std::vector<batch_entry>& roots,
    const std::vector<page_change>& pages) {
    return commit(roots, pages, true);
}

db::batch_status rdb::commit(const std::vector<batch_entry>& roots,
    const std::vector<page_change>& pages, bool preserve_pages) {
    try {
        size_t selected = stores_.size();
        const auto select = [&](const std::string& key) {
            const auto shard = locate(key);
            if (selected == stores_.size()) selected = shard;
            return selected == shard;
        };
        // Independent DB instances have independent WALs. Decline before any
        // write instead of claiming atomicity across configured path shards.
        for (const auto& root : roots)
            if (root.action != update_action::keep && !select(root.key))
                return batch_status::unsupported;
        for (const auto& page : pages)
            if (!select(page.owner)) return batch_status::unsupported;
        if (selected == stores_.size()) return batch_status::ok;
        auto& storage = *stores_[selected];
        rocksdb::WriteBatch batch;
        for (const auto& root : roots) {
            if (root.action == update_action::keep) continue;
            if (!preserve_pages || root.clear_pages ||
                root.action == update_action::erase) {
                const auto prefix = rdb_address::prefix(root.key);
                const auto status = batch.DeleteRange(storage.pages, prefix,
                    rdb_address::end(prefix));
                if (!status.ok()) return batch_status::error;
            }
            rocksdb::Status status;
            if (root.action == update_action::write) {
                status = batch.Put(storage.roots, root.key, root.value);
                if (status.ok() && !root.found) status = batch.Put(storage.scan,
                    rdb_address::index(root.key), "");
            } else {
                status = batch.Delete(storage.roots, root.key);
                if (status.ok()) status = batch.Delete(storage.scan,
                    rdb_address::index(root.key));
            }
            if (!status.ok()) return batch_status::error;
        }
        for (const auto& page : pages) {
            const auto address = rdb_address::page(page.owner, page.id);
            const auto status = page.action == update_action::write ?
                batch.Put(storage.pages, address, page.value) :
                batch.Delete(storage.pages, address);
            if (!status.ok()) return batch_status::error;
        }
        const auto status = storage.data->Write(rocksdb::WriteOptions(),
            &batch);
        if (status.ok()) {
            const auto delta = root_count_delta(roots);
            if (delta) storage.root_count.fetch_add(
                static_cast<uint64_t>(delta), std::memory_order_relaxed);
            return batch_status::ok;
        }
        logger_error("RocksDB batch failed: %s", status.ToString().c_str());
        return batch_status::error;
    } catch (const std::bad_alloc&) { return batch_status::error; }
}

} // namespace pkv
#endif
