#include "stdafx.h"
#include "json_hash.h"

namespace pkv {
namespace dao {
bool json_hash::hgetdel(shared_db& db, const std::string& key,
    const hash_fields_parser& parse, hash_field_values& out) {
    out.clear(); reset();
    bool valid = true;
    bool ok;
    try {
        ok = db->update(key, [&](bool found, std::string& bytes) {
            const bool expired = found && !decode(db, bytes);
            if (expired && read_status_ != read_status::missing) {
                valid = false; return db::update_action::keep;
            }
            if (!parse_fields()) {
                valid = false; return db::update_action::keep;
            }
            std::vector<std::string> names;
            if (!parse(names)) {
                valid = false; return db::update_action::keep;
            }
            out.resize(names.size());
            bool changed = false;
            for (size_t i = 0; i < names.size(); ++i) {
                auto it = record_.hash_value.find(names[i]);
                if (it == record_.hash_value.end()) continue;
                out[i].found = true;
                out[i].value.swap(it->second);
                record_.hash_value.erase(it);
                changed = true;
            }
            if (expired) return db::update_action::erase;
            if (!changed) return db::update_action::keep;
            if (record_.hash_value.empty()) return db::update_action::erase;
            if (!encode(db, record_, bytes)) {
                valid = false; return db::update_action::keep;
            }
            return db::update_action::write;
        });
    } catch (const std::bad_alloc&) {
        out.clear(); read_status_ = read_status::storage_error;
        return false;
    }
    if (!ok) read_status_ = read_status::storage_error;
    if (!ok || !valid) { out.clear(); return false; }
    read_status_ = read_status::ok;
    return true;
}
} // namespace dao
} // namespace pkv
