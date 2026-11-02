#include "db.h"
#include "page_transaction.h"
#include "coder/record_codec.h"
#include <numeric>
#include <stdexcept>

namespace pkv {
key_wait_token db::wait_for_lists(const std::vector<std::string>& keys) {
    if (!ready_registry_)
        throw std::logic_error("actor-local DB cannot wait for lists");
    return ready_registry_->subscribe(keys, key_wait_kind::list);
}
key_wait_token db::wait_for_zsets(const std::vector<std::string>& keys) {
    if (!ready_registry_)
        throw std::logic_error("actor-local DB cannot wait for sorted sets");
    return ready_registry_->subscribe(keys, key_wait_kind::zset);
}
key_ready_change db::ready_change(const std::string& key,
    const std::string* value) {
    const auto& registry = ready_registry_;
    if (!registry || !registry->subscribers())
        return {nullptr, false, -1};
    record_header header;
    const auto status = value ? codec_->stored_header(*value, header) :
        codec_status::invalid_record;
    if (status == codec_status::out_of_memory) throw std::bad_alloc();
    const bool ready = status == codec_status::ok &&
        (header.kind == value_kind::zset || header.kind == value_kind::list) &&
        (header.expires_at_ms < 0 || header.expires_at_ms > unix_time_ms());
    return {&key, ready, header.expires_at_ms, header.kind ==
        value_kind::zset ? key_wait_kind::zset : key_wait_kind::list};
}
bool db::apply_root(const std::string& key, const std::string* value) {
    const auto ready = ready_change(key, value);
    if (ready.key) ready_registry_->prepare(&ready, 1);
    const bool ok = value ? dbset(key, *value) : dbdel(key);
    if (ok && ready.key) ready_registry_->publish(&ready, 1);
    return ok;
}
std::vector<key_ready_change> db::prepare_ready(
    const std::vector<batch_entry>& roots, const page_update* ordered,
    size_t count, const std::vector<page_change>* pages) {
    std::vector<key_ready_change> changes;
    const auto& registry = ready_registry_;
    if (!registry || !registry->subscribers()) return changes;
    changes.reserve(roots.size());
    for (const auto& root : roots) {
        const auto page = pages ? std::lower_bound(pages->begin(), pages->end(),
            root.key, [](const page_change& a, const std::string& b) {
            return a.owner < b;
        }) : std::vector<page_change>::const_iterator();
        const bool edited = root.action != update_action::keep ||
            (pages && page != pages->end() && page->owner == root.key);
        if (!edited || (root.maintenance_only && root.found)) {
            changes.push_back({nullptr, false, -1}); continue;
        }
        const auto* bytes = root.action == update_action::erase ? nullptr :
            &root.value;
        std::string original;
        if (root.action == update_action::keep && root.value.empty()) {
            const auto status = dblookup(root.key, original);
            if (status == lookup_status::error) throw std::bad_alloc();
            bytes = status == lookup_status::found ? &original : nullptr;
        }
        changes.push_back(ready_change(root.key, bytes));
    }
    if (!count || roots.size() < 2) return changes;
    std::vector<size_t> ranks(roots.size(), SIZE_MAX);
    size_t rank = 0;
    for (size_t i = 0; i < count; ++i) {
        const auto ready_types = ordered[i].ready_types == ~0U ?
            key_wait_registry::waiting_types() : ordered[i].ready_types;
        for (const auto& root : ordered[i].roots) {
            if (root.action != update_action::write) continue;
            const auto it = std::lower_bound(roots.begin(), roots.end(),
                root.key, [](const batch_entry& a, const std::string& b) {
                return a.key < b;
            });
            if (it == roots.end() || it->key != root.key) continue;
            const auto position = static_cast<size_t>(it - roots.begin());
            if (ranks[position] != SIZE_MAX || !changes[position].ready)
                continue;
            const auto first = ready_change(root.key, &root.value);
            if (first.ready && (ready_types &
                (1U << static_cast<unsigned>(first.kind))))
                ranks[position] = rank++;
        }
    }
    std::vector<size_t> positions(roots.size());
    std::iota(positions.begin(), positions.end(), 0);
    std::stable_sort(positions.begin(), positions.end(),
        [&](size_t a, size_t b) { return ranks[a] < ranks[b]; });
    std::vector<key_ready_change> result;
    result.reserve(changes.size());
    for (const auto position : positions) result.push_back(changes[position]);
    return result;
}
} // namespace pkv
