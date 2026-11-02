#pragma once
#include "dao_base.h"
#include "db/string_store.h"

namespace pkv {
namespace dao {
inline dao_base::read_status string_read_status(string_store::status status) {
    using result = dao_base::read_status;
    switch (status) {
    case string_store::status::ok: return result::ok;
    case string_store::status::unsupported: return result::unsupported_format;
    case string_store::status::storage: return result::storage_error;
    case string_store::status::too_large: return result::string_too_large;
    default: return result::invalid_record;
    }
}
} // namespace dao
} // namespace pkv
