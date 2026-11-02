#pragma once
#include "dao_base.h"
#include "db/map_store.h"

namespace pkv {
namespace dao {

inline dao_base::read_status map_read_status(map_store::status status) {
    using result = dao_base::read_status;
    switch (status) {
    case map_store::status::ok: return result::ok;
    case map_store::status::wrong_kind: return result::wrong_type;
    case map_store::status::unsupported: return result::unsupported_format;
    case map_store::status::storage: return result::storage_error;
    case map_store::status::too_large: return result::string_too_large;
    default: return result::invalid_record;
    }
}

} // namespace dao
} // namespace pkv
