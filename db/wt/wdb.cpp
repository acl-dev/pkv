#include "stdafx.h"
#ifdef HAS_WT
#include "wt.h"
#include "db/key_count.h"
#include "wdb.h"
#include "db/page_transaction.h"
#include <limits>

namespace pkv {

wdb::wdb(size_t cache_max) : cache_max_(cache_max) {}
wdb::~wdb() = default;
size_t wdb::locate(const std::string& key) const {
    return static_cast<size_t>(acl_hash_crc32(key.data(), key.size())) %
        dbs_.size();
}
bool wdb::dbopen(const char* paths) {
    if (!paths || !dbs_.empty()) return false;
    acl::string input(paths);
    std::vector<std::string> roots;
    for (const auto& path : input.split2(";, \t"))
        roots.emplace_back(path);
    return dbopen_paths(roots);
}

bool wdb::dbopen_paths(const std::vector<std::string>& paths) {
    if (paths.empty() || !dbs_.empty()) return false;
    for (const auto& path : paths)
        if (path.empty() || path.find('\0') != path.npos) return false;
    for (const auto& path : paths)
        if (!open_one(path + "/wdb")) return false;
    return true;
}
bool wdb::open_one(const std::string& path) {
    if (dbs_.size() >= std::numeric_limits<unsigned long long>::max() / 256)
        return false;
    if (acl_make_dirs(path.c_str(), 0755) == -1) return false;
    std::unique_ptr<wt> database(new wt(cache_max_));
    if (!database->open(path.c_str())) return false;
    dbs_.push_back(std::move(database));
    return true;
}
bool wdb::dbget(const std::string& key, std::string& out) {
    return dblookup(key, out) == lookup_status::found;
}
db::lookup_status wdb::dblookup(const std::string& key, std::string& out) {
    return dbs_[locate(key)]->lookup(key, out);
}
db::lookup_status wdb::dblookup_page(const std::string& owner, uint64_t id,
    std::string& out) {
    return dbs_[locate(owner)]->page(owner, id, out);
}
bool wdb::dbset(const std::string& key, const std::string& value) {
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
bool wdb::dbdel(const std::string& key) {
    try {
        std::vector<batch_entry> roots;
        std::string previous;
        if (dblookup(key, previous) != lookup_status::found) return false;
        roots.emplace_back(key, true, "");
        roots.back().action = update_action::erase;
        return commit(roots, {}, false) == batch_status::ok;
    } catch (const std::bad_alloc&) { return false; }
}
db::batch_status wdb::dbapply(const std::vector<batch_entry>& roots) {
    return commit(roots, {}, false);
}
db::batch_status wdb::dbapply_pages(const std::vector<batch_entry>& roots,
    const std::vector<page_change>& pages) {
    return commit(roots, pages, true);
}
db::batch_status wdb::commit(const std::vector<batch_entry>& roots,
    const std::vector<page_change>& pages, bool preserve) {
    size_t selected = dbs_.size();
    const auto select = [&](const std::string& key) {
        const auto shard = locate(key);
        if (selected == dbs_.size()) selected = shard;
        return selected == shard;
    };
    for (const auto& root : roots)
        if (root.action != update_action::keep && !select(root.key))
            return batch_status::unsupported;
    for (const auto& page : pages)
        if (!select(page.owner)) return batch_status::unsupported;
    return selected == dbs_.size() ? batch_status::ok :
        dbs_[selected]->apply(roots, pages, preserve);
}
db::count_status wdb::dbkey_count(uint64_t& out) {
    uint64_t count = 0;
    for (const auto& store : dbs_)
        if (!add_key_count(store->key_count(), count))
            return count_status::error;
    out = count; return count_status::ok;
}
db::scan_status wdb::dbscan_keys(unsigned long long cursor, size_t count,
    std::vector<std::string>& keys, unsigned long long& next) {
    keys.clear(); next = 0;
    if (cursor / 256 >= dbs_.size()) return scan_status::ok;
    const auto limit = static_cast<unsigned long long>(dbs_.size()) * 256;
    while (cursor < limit) {
        const auto result = dbs_[static_cast<size_t>(cursor / 256)]->scan(
            static_cast<unsigned>(cursor % 256), keys);
        if (result != scan_status::ok) { keys.clear(); return result; }
        ++cursor;
        if (keys.size() >= (count ? count : 1)) break;
    }
    next = cursor < limit ? cursor : 0;
    return scan_status::ok;
}
bool wdb::dbscan(size_t, db_cursor&, std::vector<std::string>&, size_t) {
    return false;
}

} // namespace pkv
#endif
