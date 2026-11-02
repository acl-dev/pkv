#pragma once
#include "dao_base.h"
#include "db/list_store.h"
#include "db/string_store.h"
#include "db/zset_store.h"

namespace pkv {
namespace dao {

// Import a legacy decoded view inside the caller's existing page transaction.
// Every stored root/page remains bounded; callbacks consume members in batches.
// The caller owns logical change/maintenance flags and must finish the store.
bool import_string(record_view& view, string_store& store,
    dao_base::read_status& error);
bool import_list(record_view& view, list_store& store,
    dao_base::read_status& error);
bool import_hash(record_view& view, map_store& store,
    dao_base::read_status& error);
bool import_set(record_view& view, map_store& store,
    dao_base::read_status& error);
bool import_zset(record_view& view, zset_store& store,
    dao_base::read_status& error);

// Select the native type, import and finish it without a separate commit.
bool import_native(record_view& view, page_transaction& transaction,
    const std::string& key, dao_base::read_status& error);

} // namespace dao
} // namespace pkv
