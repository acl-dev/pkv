#pragma once
#include "list.h"
#include "db/list_store.h"
#include <functional>

namespace pkv {
namespace compact_list_root { class view; }
namespace dao {

// List DAO for native bounded pages. It never falls back to whole-value writes
// when the selected backend lacks page transactions.
class paged_list : public list {
public:
    explicit paged_list(db::page_lock_scope* scope = nullptr) : scope_(scope) {}
    bool push(shared_db& db, const std::string& key,
        const std::vector<std::string>& values, bool left, bool only_existing,
        long long& length) override;
    bool push(shared_db& db, const std::string& key, list_values_view values,
        bool left, bool only_existing, long long& length);
    bool pop(shared_db& db, const std::string& key, bool left, long long count,
        list_result& out) override;
    bool pop_one(shared_db& db, const std::string& key, bool left,
        std::string& out, bool& found);
    bool length(shared_db& db, const std::string& key, long long& out) override;
    bool range(shared_db& db, const std::string& key, long long start,
        long long end, std::vector<std::string>& out) override;
    bool index(shared_db& db, const std::string& key, const std::string& index,
        list_result& out) override;
    bool set(shared_db& db, const std::string& key, const std::string& index,
        const std::string& value) override;
    bool trim(shared_db& db, const std::string& key, long long start,
        long long end) override;
    bool insert(shared_db& db, const std::string& key, bool before,
        const std::string& pivot, const std::string& value,
        long long& length) override;
    bool remove(shared_db& db, const std::string& key, long long count,
        const std::string& value, long long& removed) override;
    bool positions(shared_db& db, const std::string& key,
        const std::string& value, const list_search& options,
        std::vector<long long>& out) override;
    bool move(shared_db& db, const std::string& source,
        const std::string& destination, bool from_left, bool to_left,
        list_result& out) override;
    bool mpop(shared_db& db, const std::vector<std::string>& keys, bool left,
        long long count, std::string& selected_key, list_result& out) override;
    using key_predicate = std::function<bool(const std::string&)>;
    using before_probe = std::function<bool()>;
    bool move_checked(shared_db& db, const std::string& source,
        const std::string& destination, bool from_left, bool to_left,
        list_result& out, const key_predicate& eligible,
        const before_probe& before = before_probe());
    bool mpop_checked(shared_db& db, const std::vector<std::string>& keys,
        bool left, long long count, std::string& selected_key, list_result& out,
        const key_predicate& eligible,
        const before_probe& before = before_probe());

private:
    db::page_lock_scope* scope_;
    struct single_push {
        const std::string* value;
        bool left;
        long long& length;
    };
    using operation = std::function<bool(list_store&)>;
    // These private operations run synchronously and never retain callbacks.
    template <class Callback>
    bool run(shared_db& db, const std::string& key, Callback&& callback) {
        const operation view(std::ref(callback));
        return run_callback(db, key, view);
    }
    using packed_operation = std::function<bool(db::batch_entry&,
        const compact_list_root::view*)>;
    // Both targets live until this synchronous transaction returns, including
    // fiber suspension. Borrow them instead of allocating owning closures.
    template <class Callback, class Packed>
    bool run_packed(shared_db& db, const std::string& key,
        Callback&& callback, Packed&& packed, const single_push* push = nullptr) {
        const operation callback_view(std::ref(callback));
        const packed_operation packed_view(std::ref(packed));
        return run_callback(db, key, callback_view, packed_view, push);
    }
    bool run_callback(shared_db& db, const std::string& key,
        const operation& callback, const packed_operation& packed = {},
        const single_push* push = nullptr);
    bool open(shared_db& db, list_store& store, long long now);
    bool import(list_store& store);
    bool store_result(list_store& store);
    bool batch_result(db::batch_status status);
};

} // namespace dao
} // namespace pkv
