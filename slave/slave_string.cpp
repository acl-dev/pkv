#include "stdafx.h"
#include "slave_client.h"
#include "rpc/page_batch.h"
#include "coder/record_codec.h"
#include "coder/string_page.h"

namespace pkv {

// Called with queue_lock_ held. Convert only fully inline String roots. Old
// peers retain per-root events; PKV2 peers always keep the native transaction.
bool slave_client::legacy_pages(const kv_message& message) {
    const auto batch = message.get_pages();
    if (!batch || !batch->pages().empty()) return false;
    size_t reserved_bytes = 0, reserved_records = 0;
    size_t pushed_bytes = 0, pushed_records = 0;
    std::vector<std::unique_ptr<kv_message>> converted;
    try {
        for (const auto& root : batch->roots()) {
            if (root.action == db::update_action::keep) continue;
            std::string bytes;
            const bool erase = root.action == db::update_action::erase;
            if (!erase) {
                string_root value;
                if (string_page_codec::decode_root(root.value, value) !=
                    string_page_status::ok || value.pages) break;
                value_record record;
                record.header.expires_at_ms = value.expires_at_ms;
                record.string_value = std::move(value.inline_value);
                if (default_record_codec()->encode(record, bytes) !=
                    codec_status::ok) break;
            }
            std::unique_ptr<kv_message> item(new kv_message(root.key,
                std::move(bytes), erase ? message_oper_del : message_oper_set,
                message.get_database()));
            if (!budget_.reserve_encoded(item->wire_bytes(), 1)) break;
            reserved_bytes += item->wire_bytes(); ++reserved_records;
            converted.push_back(std::move(item));
        }
        if (converted.size() == batch->roots().size()) {
            for (auto& item : converted) {
                const auto bytes = item->wire_bytes();
                if (!box_.push(item.get())) break;
                item.release(); pushed_bytes += bytes; ++pushed_records;
            }
            if (pushed_records == reserved_records) return true;
        }
    } catch (const std::bad_alloc&) {
        logger_error("Replica inline String conversion allocation failed");
    }
    if (reserved_records > pushed_records)
        budget_.release_encoded(reserved_bytes - pushed_bytes,
            reserved_records - pushed_records);
    // The caller closes the queue before unlocking. A consumer waiting for
    // closed() cannot send any partially queued replacement after failure.
    return false;
}

} // namespace pkv
