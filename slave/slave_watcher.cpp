//
// Created by shuxin zheng on 2023/9/25.
//

#include "stdafx.h"
#include "common/message_box.h"
#include "slave_watcher.h"
#include "coder/json_record_codec.h"
#include "coder/compact_zset.h"
#include "coder/compact_set.h"
#include "coder/compact_hash.h"
#include "coder/compact_list_root.h"
#include "coder/string_page.h"
#include "rpc/page_batch.h"
#include "common/execution_gate.h"

namespace pkv {

std::vector<shared_client> slave_watcher::invalidate_locked() {
    ++epoch_;
    std::vector<shared_client> clients;
    std::lock_guard<std::mutex> guard(clients_lock_);
    clients.swap(clients_);
    clients_count_.store(0);
    return clients;
}

bool slave_watcher::publish(const std::string& key, const std::string& value,
        message_oper_t operation, bool ok, unsigned database) {
    if (!ok || (clients_count_.load() == 0 && !collecting_.load())) {
        return true;
    }
    std::string compatible;
    const bool compact = operation == message_oper_set &&
        string_page_codec::compact_inline(value);
    const auto& payload = compact ? compatible : value;
    bool valid = true;
    if (compact) {
        try {
            compatible = value;
            valid = translate_compact_root(compatible);
        } catch (const std::bad_alloc&) { valid = false; }
    }
    std::vector<shared_client> rejected;
    {
        std::lock_guard<std::mutex> guard(queue_lock_);
        if (eof_) {
            return true;
        }
        if (exec_active_) {
            if (valid) capture_root_locked(database, key, payload, operation);
            else exec_failed_ = true;
            return valid;
        }
        if (!valid) {
            rejected = invalidate_locked();
        } else if (!budget_.reserve(key.size(), payload.size())) {
            logger_error("Replica publish queue limit exceeded: bytes=%zu, "
                "messages=%zu", budget_.bytes(), budget_.messages());
            rejected = invalidate_locked();
        } else {
            try {
                std::unique_ptr<kv_message> message(new kv_message(key, payload,
                    operation, database));
                std::unique_ptr<publication> item(new publication(message.get(),
                    epoch_));
                message.release();
                if (box_.push(item.get())) {
                    item.release();
                    return true;
                }
            } catch (const std::bad_alloc&) {
                logger_error("Replica publish allocation failed");
            }
            budget_.release(key.size(), payload.size());
            rejected = invalidate_locked();
        }
    }
    // Never call a fiber-aware recipient while holding publisher metadata
    // locks.
    for (const auto& client : rejected) {
        client->disconnect();
    }
    return true;
}

bool slave_watcher::requires_page_payload() const {
    return clients_count_.load() != 0 || collecting_.load();
}

bool slave_watcher::is_compact_root(const std::string& bytes) {
    return compact_zset::matches(bytes) || compact_set::matches(bytes) ||
        compact_hash::matches(bytes) || compact_list_root::matches(bytes) ||
        string_page_codec::compact_inline(bytes);
}

bool slave_watcher::translate_compact_root(std::string& bytes) {
    if (string_page_codec::compact_inline(bytes)) {
        string_root root;
        return string_page_codec::decode_root(bytes, root) == string_page_status::ok &&
            string_page_codec::encode_inline(-1, root.inline_value, bytes) ==
                string_page_status::ok;
    }
    value_record record;
    if (compact_zset::matches(bytes)) {
        const compact_zset::view packed(bytes);
        record.header.kind = value_kind::zset;
        record.header.expires_at_ms = packed.deadline();
        for (const auto& item : packed.entries())
            record.zset_value.emplace(item.first, item.second);
    } else if (compact_set::matches(bytes)) {
        const compact_set::view packed(bytes);
        record.header.kind = value_kind::set;
        record.header.expires_at_ms = packed.deadline();
        for (unsigned i = 0; i < packed.count(); ++i)
            record.set_value.emplace(packed.at(i).str());
    } else if (compact_hash::matches(bytes)) {
        const compact_hash::view packed(bytes);
        record.header.kind = value_kind::hash;
        record.header.expires_at_ms = packed.deadline();
        for (unsigned i = 0; i < packed.count(); ++i) {
            const auto pair = packed.at(i);
            record.hash_value.emplace(pair.first.str(), pair.second.str());
        }
    } else if (compact_list_root::matches(bytes)) {
        const compact_list_root::view packed(bytes);
        record.header.kind = value_kind::list;
        record.header.expires_at_ms = packed.deadline();
        record.list_value.reserve(packed.count());
        for (unsigned i = 0; i < packed.count(); ++i)
            record.list_value.push_back(packed.at(i).str());
    } else {
        return true;
    }
    return json_record_codec().encode(record, bytes) == codec_status::ok;
}

bool slave_watcher::on_pages_database(unsigned database,
    const std::vector<db::batch_entry>& roots,
    const std::vector<page_change>& pages) {
    if (clients_count_.load() == 0 && !collecting_.load()) {
        return true;
    }
    const auto packed_root = [](const db::batch_entry& root) {
        return root.action == db::update_action::write &&
            is_compact_root(root.value);
    };
    std::vector<db::batch_entry> compatible;
    bool translated = true;
    if (std::any_of(roots.begin(), roots.end(), packed_root)) {
        try {
            compatible = std::vector<db::batch_entry>(roots);
            for (auto& root : compatible) {
                if (!packed_root(root)) continue;
                if (!translate_compact_root(root.value)) {
                    translated = false;
                    break;
                }
                // A replica may have already migrated an earlier JSON value.
                root.clear_pages = true;
            }
        } catch (const std::bad_alloc&) {
            translated = false;
        } catch (const std::runtime_error&) {
            translated = false;
        }
    }
    const auto& wire_roots = compatible.empty() ? roots : compatible;
    page_batch_size size;
    const bool valid = translated &&
        page_batch::measure(database, wire_roots, pages, size);
    std::vector<shared_client> rejected;
    {
        std::lock_guard<std::mutex> guard(queue_lock_);
        if (eof_) {
            return true;
        }
        if (exec_active_) {
            if (!translated) exec_failed_ = true;
            else capture_pages_locked(database, wire_roots, pages,
                valid ? &size : nullptr);
            return true;
        }
        if (!valid || !budget_.reserve_encoded(size.bytes, size.records,
            size.expanded_bytes, size.actions)) {
            logger_error("Replica page publication exceeds queue limits");
            rejected = invalidate_locked();
        } else {
            try {
                auto batch = page_batch::capture(database, wire_roots, pages);
                if (batch) {
                    std::unique_ptr<kv_message> message(new kv_message(batch));
                    std::unique_ptr<publication> item(new publication(
                        message.get(), epoch_));
                    message.release();
                    if (box_.push(item.get())) {
                        item.release();
                        return true;
                    }
                }
            } catch (const std::bad_alloc&) {
                logger_error("Replica page publication allocation failed");
            }
            budget_.release_encoded(size.bytes, size.records,
                size.expanded_bytes, size.actions);
            rejected = invalidate_locked();
        }
    }
    for (const auto& client : rejected) {
        client->disconnect();
    }
    return true;
}

size_t slave_watcher::pending_bytes() {
    std::lock_guard<std::mutex> guard(queue_lock_);
    return budget_.bytes();
}

size_t slave_watcher::pending_messages() {
    std::lock_guard<std::mutex> guard(queue_lock_);
    return budget_.messages();
}

bool slave_watcher::on_set_database(unsigned database, const std::string& key,
    const std::string& value, bool ok) {
    return publish(key, value, message_oper_set, ok, database);
}

bool slave_watcher::on_del_database(unsigned database, const std::string& key,
    bool ok) {
    return publish(key, "", message_oper_del, ok, database);
}

bool slave_watcher::on_set(const std::string& key, const std::string& value,
    bool ok) {
    return publish(key, value, message_oper_set, ok);
}

bool slave_watcher::on_get(const std::string&, const std::string&, bool) {
    // Reads do not change replica state and must not amplify the write stream.
    return true;
}

bool slave_watcher::on_del(const std::string& key, bool ok) {
    return publish(key, "", message_oper_del, ok);
}

void slave_watcher::stop() {
    std::lock_guard<std::mutex> guard(queue_lock_);
    if (!eof_) {
        eof_ = true;
        box_.push(new publication(nullptr, epoch_));
    }
}

void slave_watcher::run() {
    while (auto pointer = box_.pop(-1)) {
        std::unique_ptr<publication> item(pointer);
        if (!item->message) {
            break;
        }
        forward_message(*item);
        std::lock_guard<std::mutex> guard(queue_lock_);
        budget_.release_encoded(item->message->wire_bytes(),
            item->message->wire_records(), item->message->expanded_bytes(),
            item->message->expanded_records());
    }
}

void slave_watcher::forward_message(const publication& item) {
    std::vector<shared_client> clients;
    {
        std::lock_guard<std::mutex> queue_guard(queue_lock_);
        if (item.epoch != epoch_) {
            return;
        }
        std::lock_guard<std::mutex> clients_guard(clients_lock_);
        clients = clients_;
    }
    for (const auto& client : clients) {
        item.message->refer(1);
        client->push(item.message);
    }
}

void slave_watcher::add_client(const shared_client& client) {
    // Drain admitted writers before enabling payload observation.
    execution_guard admission(true);
    bool stopped;
    {
        std::lock_guard<std::mutex> queue_guard(queue_lock_);
        stopped = eof_;
        if (!stopped) {
            std::lock_guard<std::mutex> clients_guard(clients_lock_);
            clients_.emplace_back(client);
            clients_count_.store(clients_.size());
        }
    }
    if (stopped) {
        client->disconnect();
    }
}

bool slave_watcher::del_client(const shared_client& client) {
    std::lock_guard<std::mutex> guard(clients_lock_);
    for (auto it = clients_.begin(); it != clients_.end(); ++it) {
        if ((*it).get() == client.get()) {
            clients_.erase(it);
            clients_count_.store(clients_.size());
            return true;
        }
    }

    // An overflow may already have detached this subscriber.
    return false;
}

} // namespace pkv
