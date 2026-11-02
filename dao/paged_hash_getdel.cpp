#include "stdafx.h"
#include "paged_hash.h"

namespace pkv {
namespace dao {
bool paged_hash::hgetdel(shared_db& db, const std::string& key,
    const hash_fields_parser& parse, hash_field_values& out) {
    out.clear();
    bool ok;
    try {
        ok = run(db, key, [&](map_store& store) {
            std::vector<std::string> names;
            if (!parse(names)) return false;
            out.resize(names.size());
            for (size_t i = 0; i < names.size(); ++i)
                if (!store.take(names[i], out[i].value, out[i].found))
                    return false;
            return true;
        });
    } catch (const std::bad_alloc&) {
        out.clear(); read_status_ = read_status::storage_error;
        return false;
    }
    if (!ok) out.clear();
    return ok;
}
} // namespace dao
} // namespace pkv
