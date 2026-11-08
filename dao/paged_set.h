#pragma once
#include "set.h"
#include "map_status.h"
#include <functional>

namespace pkv {
namespace compact_set { class view; }
namespace dao {

class paged_set : public set {
public:
    explicit paged_set(db::page_lock_scope* scope = nullptr) : scope_(scope) {}
    bool change_one(shared_db& db, const std::string& key,
        const std::string& member, bool add, long long& changed);
    bool change(shared_db& db, const std::string& key,
        const std::vector<std::string>& members, bool add,
        long long& changed) override;
    bool change_members(shared_db& db, const std::string& key,
        const map_store::set_member_at& member_at, size_t count, bool add,
        long long& changed);
    bool size(shared_db& db, const std::string& key,
        long long& count) override;
    bool membership(shared_db& db, const std::string& key,
        const std::string* const* members, size_t count, unsigned& found);
    bool membership(shared_db& db, const std::string& key,
        const std::vector<std::string>& members,
        std::vector<long long>& out) override;
    bool members(shared_db& db, const std::string& key,
        std::set<std::string>& out) override;
    using member_visitor = std::function<void(uint64_t, const std::string&)>;
    using packed_visitor = std::function<void(const compact_set::view&)>;
    bool visit_members(shared_db& db, const std::string& key,
        const member_visitor& visit, const packed_visitor& visit_packed = {});
    bool move(shared_db& db, const std::string& source,
        const std::string& destination, const std::string& member,
        bool& moved) override;
    bool algebra(shared_db& db, const std::vector<std::string>& keys,
        set_operation operation, const std::string* destination,
        std::set<std::string>& out) override;
    bool intersection_size(shared_db& db,
        const std::vector<std::string>& keys, long long limit,
        long long& count) override;
    bool sample(shared_db& db, const std::string& key, long long count,
        bool remove, set_sample_result& out) override;
private:
    db::page_lock_scope* scope_;
    using operation = std::function<bool(map_store&)>;
    using stores = std::map<std::string, std::unique_ptr<map_store>>;
    // These private operations run synchronously and never retain callbacks.
    template <class Callback>
    bool run(shared_db& db, const std::string& key, Callback&& callback,
        bool skip_missing = false) {
        const operation view(std::ref(callback));
        return run_callback(db, key, view, skip_missing);
    }
    using packed_operation = std::function<bool(db::batch_entry&,
        const compact_set::view*)>;
    using promotion_operation = std::function<bool(map_store&,
        const compact_set::view&)>;
    bool run_callback(shared_db& db, const std::string& key,
        const operation& callback, bool skip_missing,
        const packed_operation& packed = {},
        const promotion_operation& promote = {});
    bool open(shared_db& db, page_transaction& tx, const std::string& key,
        map_store& store, long long now);
    bool import(map_store& store);
    bool result(db::batch_status status);
    bool finish(map_store& store, bool pack_set = false);
    bool sources(shared_db& db, page_transaction& tx,
        const std::vector<std::string>& keys, stores& out);
    bool contains(map_store& store, const std::string& member, bool& found);
    bool accept(stores& sources, const std::string& first,
        const std::string& member, set_operation operation, bool& accepted);
    bool compare_grouped_sets(map_store& left, map_store& right,
        set_operation operation,
        const std::function<void(const std::string&)>& emit);
    bool choose(map_store& store, long long count,
        std::vector<std::string>& out);
    bool visit_members(map_store& store, const member_visitor& visit);
};

} // namespace dao
} // namespace pkv
