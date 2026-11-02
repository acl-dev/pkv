#include "stdafx.h"
#ifdef HAS_ROCKSDB
#include "rdb_storage.h"
#include "db/page_transaction.h"
#include "rpc/page_wire.h"

namespace pkv {
db::lookup_status rdb::dblookup_bounded_root(const std::string& owner,
    std::string& out) {
    if (stores_.empty()) return lookup_status::error;
    try {
        auto& storage = *stores_[locate(owner)];
        rocksdb::PinnableSlice value;
        const auto status = storage.data->Get(rocksdb::ReadOptions(),
            storage.roots, owner, &value);
        if (status.IsNotFound()) return lookup_status::missing;
        if (!status.ok() || value.size() > page_transaction::max_value_bytes)
            return lookup_status::error;
        std::string bytes(value.empty() ? "" : value.data(), value.size());
        out.swap(bytes); return lookup_status::found;
    } catch (const std::bad_alloc&) { return lookup_status::error; }
}
db::batch_status rdb::dbvisit_pages(const std::string& owner,
    const page_visitor& visit) {
    if (stores_.empty()) return batch_status::error;
    try {
        auto& storage = *stores_[locate(owner)];
        const auto prefix = rdb_address::prefix(owner);
        const auto end = rdb_address::end(prefix);
        const rocksdb::Slice limit(end);
        rocksdb::ReadOptions options;
        options.iterate_upper_bound = &limit; options.fill_cache = false;
        std::unique_ptr<rocksdb::Iterator> cursor(
            storage.data->NewIterator(options, storage.pages));
        for (cursor->Seek(prefix); cursor->Valid(); cursor->Next()) {
            const auto key = cursor->key(), value = cursor->value();
            if (!key.starts_with(prefix) || key.size() != prefix.size() + 8 ||
                !page_wire::get64(key.data() + prefix.size()) ||
                value.size() > page_transaction::max_value_bytes)
                return batch_status::error;
            const std::string bytes(value.empty() ? "" : value.data(),
                value.size());
            if (!visit(page_wire::get64(key.data() + prefix.size()), bytes))
                return batch_status::aborted;
        }
        return cursor->status().ok() ? batch_status::ok : batch_status::error;
    } catch (const std::bad_alloc&) { return batch_status::error; }
}
} // namespace pkv
#endif
