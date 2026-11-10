#pragma once
#include "hash.h"
#include "map_status.h"
#include <functional>

namespace pkv {
namespace compact_hash { class view; }
namespace dao {

class paged_hash : public hash {
public:
    explicit paged_hash(db::page_lock_scope* scope = nullptr) : scope_(scope) {}
    bool hgetdel(shared_db& db, const std::string& key,
        const hash_fields_parser& parse, hash_field_values& out) override;
    int hdel(shared_db& db, const std::string& key,
        const std::string& name) override;
    int hdel(shared_db& db, const std::string& key,
        const std::vector<std::string>& names) override;
    bool hset(shared_db& db, const std::string& key, const std::string& name,
        const std::string& value) override;
    // Publish the insertion flag only after the transaction commits.
    bool hset(shared_db& db, const std::string& key, const std::string& name,
        const std::string& value, bool& added);
    bool hget(shared_db& db, const std::string& key, const std::string& name,
        std::string& value) override;
    bool hgetall(shared_db& db, const std::string& key) override;
    using field_list = std::vector<std::pair<std::string, std::string>>;
    bool hgetall(shared_db& db, const std::string& key, bool with_values,
        field_list& out);
    int hmset(shared_db& db, const std::string& key,
        map_store::hash_field_refs fields);
    int hmset_pair(shared_db& db, const std::string& key,
        map_store::hash_field_refs::value_type first,
        map_store::hash_field_refs::value_type second);
    bool hgetall(shared_db& db, const std::string& key,
        bool with_values) override;
    int hmset(shared_db& db, const std::string& key,
        const std::map<std::string, std::string>& fields) override;
    bool hmget(shared_db& db, const std::string& key,
        const std::string* const* fields, size_t count, std::string* values,
        unsigned& found);
    using field_reader = std::function<bool(const std::string&, std::string&,
        bool&)>;
    // The reader is valid only during the synchronous visit. A collector
    // can clear scan_complete to discard its staging and use point reads.
    bool read_fields(shared_db& db, const std::string& key,
        const std::function<bool(const field_reader&)>& visit,
        size_t distinct, const map_store::key_filter& filter,
        const map_store::owning_visitor& collect, bool& scan_complete);
    bool hmget(shared_db& db, const std::string& key,
        const std::vector<std::string>& names,
        std::map<std::string, std::string>& result) override;
    bool hsetnx(shared_db& db, const std::string& key, const std::string& name,
        const std::string& value, bool& added) override;
    bool hincrby(shared_db& db, const std::string& key, const std::string& name,
        long long delta, long long& value) override;
    bool hincrbyfloat(shared_db& db, const std::string& key,
        const std::string& name, const std::string& delta,
        std::string& value) override;
    bool hlen(shared_db& db, const std::string& key,
        long long& length) override;
    bool hstrlen(shared_db& db, const std::string& key, const std::string& name,
        long long& length, bool& exists) override;
    bool sample(shared_db& db, const std::string& key, long long count,
        bool with_values, sample_values& out) override;
    const std::map<std::string, std::string>& get_fields() override {
        return fields_;
    }
private:
    db::page_lock_scope* scope_;
    std::map<std::string, std::string> fields_;
    using operation = std::function<bool(map_store&)>;
    // These private operations run synchronously and never retain callbacks.
    template <class Callback>
    bool run(shared_db& db, const std::string& key, Callback&& callback) {
        const operation view(std::ref(callback));
        return run_callback(db, key, view);
    }
    using packed_operation = std::function<bool(db::batch_entry&,
        const compact_hash::view*)>;
    using promotion_operation = std::function<bool(map_store&,
        const compact_hash::view&)>;
    // These targets live until the synchronous transaction returns, including
    // fiber suspension. Reference wrappers avoid owning closure allocations.
    template <class Callback, class Packed>
    bool run_packed(shared_db& db, const std::string& key,
        Callback&& callback, Packed&& packed) {
        const operation callback_view(std::ref(callback));
        const packed_operation packed_view(std::ref(packed));
        return run_callback(db, key, callback_view, packed_view);
    }
    template <class Callback, class Packed, class Promotion>
    bool run_packed(shared_db& db, const std::string& key,
        Callback&& callback, Packed&& packed, Promotion&& promotion) {
        const operation callback_view(std::ref(callback));
        const packed_operation packed_view(std::ref(packed));
        const promotion_operation promotion_view(std::ref(promotion));
        return run_callback(db, key, callback_view, packed_view, promotion_view);
    }
    bool run_callback(shared_db& db, const std::string& key,
        const operation& callback, const packed_operation& packed = {},
        const promotion_operation& promote = {});
    bool open(shared_db& db, page_transaction& transaction,
        const std::string& key, map_store& store);
    bool transform_field(shared_db& db, const std::string& key,
        const std::string& name, uint64_t old_limit,
        const map_store::hash_transform& transform);
    bool import(map_store& store);
    bool store_result(map_store& store);
    bool choose(map_store& store, long long count, bool with_values,
        sample_values& out);
};

} // namespace dao
} // namespace pkv
