//
// Created by shuxin zheng on 2023/9/25.
//

#pragma once
#include "common/kv_message.h"
#include "db/db_watcher.h"
#include "slave_client.h"
#include "common/message_box.h"
#include "rpc/exec_batch.h"
#include <atomic>
#include <mutex>
#include <vector>

namespace pkv {

class slave_client;
class replica_scope;
struct page_batch_size;

class slave_watcher : public db_watcher {
public:
    explicit slave_watcher(size_t byte_limit = 64 * 1024 * 1024, size_t
        message_limit = 65536)
    : budget_(byte_limit, message_limit) {}
    ~slave_watcher() override = default;

public:
    void run();
    void stop();
    size_t pending_bytes();
    size_t pending_messages();

    void add_client(const shared_client& client);
    bool del_client(const shared_client& client);

protected:
    bool prepare_move(const family_move& move,
        std::unique_ptr<family_publication>& out) override;
    bool supports_pages() const override { return true; }
    bool requires_page_payload() const override;
    bool on_pages_database(unsigned database,
        const std::vector<db::batch_entry>& roots,
        const std::vector<page_change>& pages) override;
    bool on_set_database(unsigned database, const std::string& key, const
        std::string& value, bool ok) override;
    bool on_del_database(unsigned database, const std::string& key, bool ok)
        override;
    // @override
    bool on_set(const std::string& key, const std::string& value, bool ok)
        override;

    // @override
    bool on_get(const std::string& key, const std::string& value, bool ok)
        override;
    bool observes_reads() const override { return false; }

    // @override
    bool on_del(const std::string& key, bool ok) override;

private:
    static bool is_compact_root(const std::string& bytes);
    static bool translate_compact_root(std::string& bytes);
    friend class replica_scope;
    std::atomic<bool> collecting_{false};
    bool exec_active_ = false, exec_capture_ = false, exec_failed_ = false;
    size_t exec_bytes_ = 0, exec_records_ = 0;
    size_t exec_staged_bytes_ = 0, exec_staged_records_ = 0;
    size_t exec_expanded_bytes_ = 0, exec_actions_ = 0;
    unsigned long long exec_generation_ = 0;
    bool exec_preparing_ = false;
    std::vector<std::shared_ptr<const page_batch>> exec_pages_;
    std::vector<exec_entry> exec_entries_;
    struct prepared_move;
    bool begin_exec();
    void end_exec();
    void abort_exec();
    bool capture_pages_locked(unsigned database,
        const std::vector<db::batch_entry>& roots,
        const std::vector<page_change>& pages,
        const page_batch_size* measured = nullptr);
    void capture_root_locked(unsigned database, const std::string& key,
        const std::string& value, message_oper_t operation);
    std::vector<shared_client> clients_;
    std::mutex clients_lock_;
    std::mutex queue_lock_;
    std::atomic<size_t> clients_count_{0};
    bool eof_ = false;
    struct publication {
        kv_message* message;
        unsigned long long epoch;
        publication(kv_message* value, unsigned long long generation) :
            message(value), epoch(generation) {}
        ~publication() { if (message) message->unrefer(); }
    };
    mbox<publication> box_;
    replica_budget budget_;
    unsigned long long epoch_ = 0;

    bool publish(const std::string& key, const std::string& value,
        message_oper_t operation, bool ok, unsigned database = 0);

    void forward_message(const publication& item);
    std::vector<shared_client> invalidate_locked();
};

} // namespace pkv
