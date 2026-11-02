#include "stdafx.h"
#include "db.h"
#include "expiry_index.h"
#include "coder/record_codec.h"

namespace pkv {
void db::track_expiry(const std::string& key,
    const std::string* value) noexcept {
    if (!expiry_) return;
    if (!value) { expiry_->erase(key); return; }
    try {
        record_header header;
        const auto status = codec_->stored_header(*value, header);
        if (status == codec_status::ok) expiry_->set(key, header.expires_at_ms);
        else if (status == codec_status::out_of_memory) expiry_->invalidate();
        else expiry_->erase(key);
    } catch (...) { expiry_->invalidate(); }
}
db::scan_status db::rebuild_expiry() {
    execution_guard admission(true);
    try {
        std::unique_ptr<expiry_index> pending(new expiry_index);
        unsigned long long cursor = 0;
        do {
            std::vector<std::string> keys;
            unsigned long long next;
            const auto status = dbscan_keys(cursor, 256, keys, next);
            if (status != scan_status::ok) return status;
            for (const auto& key : keys) {
                std::string value;
                const auto found = dblookup(key, value);
                if (found == lookup_status::error) return scan_status::error;
                if (found == lookup_status::missing) continue;
                record_header header;
                const auto decoded = codec_->stored_header(value, header);
                if (decoded == codec_status::out_of_memory)
                    return scan_status::error;
                if (decoded == codec_status::ok &&
                    !pending->set(key, header.expires_at_ms))
                    return scan_status::error;
            }
            if (next && next == cursor) return scan_status::error;
            cursor = next;
        } while (cursor);
        expiry_.swap(pending);
        return scan_status::ok;
    } catch (const std::bad_alloc&) { return scan_status::error; }
}
db::batch_status db::expire_keys(std::vector<std::string> keys,
    size_t& removed) {
    removed = 0;
    size_t proposed = 0;
    const auto result = transact(keys, [&](batch_view roots) {
        const auto now = unix_time_ms();
        for (auto& root : roots) {
            if (!root.found) {
                if (expiry_) expiry_->erase(root.key);
                continue;
            }
            record_header header;
            const auto status = codec_->stored_header(root.value, header);
            if (status == codec_status::out_of_memory) {
                if (expiry_) expiry_->invalidate();
                return false;
            }
            if (status != codec_status::ok) {
                if (expiry_) expiry_->erase(root.key);
                continue;
            }
            if (header.expires_at_ms < 0 || header.expires_at_ms > now) {
                if (expiry_) expiry_->set(root.key, header.expires_at_ms);
                continue;
            }
            root.action = update_action::erase;
            ++proposed;
        }
        return true;
    });
    if (result == batch_status::ok) { removed = proposed; return result; }
    if (result != batch_status::unsupported || keys.size() < 2) return result;
    // Independent physical stores cannot share an atomic engine commit.
    // Each retry rechecks the current deadline under its own key lock.
    auto overall = batch_status::ok;
    for (const auto& key : keys) {
        size_t count;
        const auto status = expire_keys({key}, count);
        removed += count;
        if (status != batch_status::ok) overall = status;
    }
    return overall;
}
expiry_progress db::expire_step(size_t limit) {
    expiry_progress out;
    if (!limit) return out;
    bool rebuild;
    {
        execution_guard admission;
        rebuild = !expiry_ || expiry_->dirty();
    }
    if (rebuild) {
        const auto status = rebuild_expiry();
        if (status != scan_status::ok) {
            out.ok = false;
            out.supported = status != scan_status::unsupported;
            return out;
        }
    }
    execution_guard admission;
    std::vector<std::string> keys;
    out.more = expiry_->due(unix_time_ms(),
        std::min(limit, size_t(1024)), keys);
    out.checked = keys.size();
    if (!keys.empty())
        out.ok = expire_keys(std::move(keys), out.removed) == batch_status::ok;
    return out;
}
} // namespace pkv
