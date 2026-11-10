#include "stdafx.h"
#include "paged_hash.h"
#include "coder/compact_hash.h"

namespace pkv {
namespace dao {
bool paged_hash::hgetdel(shared_db& db, const std::string& key,
    const hash_fields_parser& parse, hash_field_values& out) {
    out.clear();
    bool ok;
    try {
        bool parsed = true;
        const auto tree = [&](map_store& store) {
            std::vector<std::string> names;
            if (!parse(names)) return false;
            out.resize(names.size());
            for (size_t i = 0; i < names.size(); ++i)
                if (!store.take(names[i], out[i].value, out[i].found))
                    return false;
            return true;
        };
        ok = run_packed(db, key, tree,
            [&](db::batch_entry& root, const compact_hash::view* packed) {
                std::vector<std::string> names;
                parsed = parse(names);
                if (!parsed) return true;
                out.resize(names.size());
                if (!packed) return true;
                uint64_t removed = 0;
                unsigned remaining = packed->count();
                for (size_t i = 0; i < names.size(); ++i) {
                    compact_hash::member_view value;
                    unsigned index;
                    if (!packed->lookup(names[i], value, &index)) continue;
                    const auto bit = uint64_t{1} << index;
                    if (removed & bit) continue;
                    out[i].value.assign(value.data(), value.size());
                    out[i].found = true;
                    removed |= bit;
                    if (!--remaining) break;
                }
                if (!removed) return true;
                if (remaining) {
                    root.value = packed->without(removed, remaining);
                    root.action = db::update_action::write;
                } else {
                    root.action = db::update_action::erase;
                }
                return true;
            }) && parsed;
    } catch (const std::bad_alloc&) {
        out.clear(); read_status_ = read_status::storage_error;
        return false;
    }
    if (!ok) out.clear();
    return ok;
}
} // namespace dao
} // namespace pkv
