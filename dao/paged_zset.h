#pragma once
#include "zset.h"
#include "db/zset_store.h"
#include "map_status.h"
#include "common/geo_search.h"
#include "coder/compact_zset.h"
#include <functional>

namespace pkv {
namespace compact_zset { class view; }
namespace dao {

class paged_zset : public zset {
public:
    explicit paged_zset(db::page_lock_scope* scope = nullptr) : scope_(scope) {}
    bool add(shared_db& db, const std::string& key,
        const std::vector<zset_member>& values, const zadd_options& options,
        zadd_result& out) override;
    bool add_one(shared_db& db, const std::string& key,
        const std::string& member, double score, const zadd_options& options,
        zadd_result& out);
    bool remove(shared_db& db, const std::string& key,
        const std::vector<std::string>& values, long long& removed) override;
    bool size(shared_db& db, const std::string& key,
        long long& count) override;
    bool score(shared_db& db, const std::string& key,
        const std::string& member, zset_score& out);
    bool scores(shared_db& db, const std::string& key,
        const std::string* const* members, size_t count, zset_score* out);
    bool scores(shared_db& db, const std::string& key,
        const std::vector<std::string>& values,
        std::vector<zset_score>& out) override;
    bool range(shared_db& db, const std::string& key, long long start,
        long long end, bool reverse, std::vector<zset_member>& out) override;
    bool select(shared_db& db, const std::string& key, const zset_query& query,
        std::vector<zset_member>& out) override;
    bool count(shared_db& db, const std::string& key, const zset_query& query,
        long long& out) override;
    bool remove_range(shared_db& db, const std::string& key,
        const zset_query& query, long long& removed) override;
    bool store_range(shared_db& db, const std::string& source,
        const std::string& destination, const zset_query& query,
        long long& stored) override;
    bool rank(shared_db& db, const std::string& key, const std::string& member,
        bool reverse, zset_rank& out) override;
    bool pop(shared_db& db, const std::string& key, long long count,
        bool maximum, std::vector<zset_member>& out) override;
    bool pop_one(shared_db& db, const std::string& key, bool maximum,
        zset_member& out, bool& found);
    bool mpop(shared_db& db, const std::vector<std::string>& keys,
        bool maximum, long long count, std::string& selected_key,
        zset_pop_result& out) override;
    using key_predicate = std::function<bool(const std::string&)>;
    using before_probe = std::function<bool()>;
    bool mpop_checked(shared_db& db, const std::vector<std::string>& keys,
        bool maximum, long long count, std::string& selected_key,
        zset_pop_result& out, const key_predicate& eligible,
        const before_probe& before);
    bool sample(shared_db& db, const std::string& key, long long count,
        zset_sample_result& out) override;
    bool algebra(shared_db& db, const std::vector<std::string>& keys,
        zset_operation operation, const std::string* destination,
        bool cardinality_only, const zset_options_parser& configure,
        std::vector<zset_member>& out, long long& cardinality) override;
    using geo_parser =
        std::function<bool(zset_store&, geo::search_options&)>;
    bool geo_search(shared_db& db, const std::string& source,
        const std::string* destination, const geo_parser& configure,
        geo::search_options& options, std::vector<geo::match>& out,
        long long& stored);
private:
    db::page_lock_scope* scope_;
    bool geo_collect(zset_store& store, const geo::search_options& options,
        std::vector<geo::match>& out);
    using operation = std::function<bool(zset_store&)>;
    // These private operations run synchronously and never retain callbacks.
    template <class Callback>
    bool run(shared_db& db, const std::string& key, Callback&& callback,
        bool skip_missing = false) {
        const operation view(std::ref(callback));
        return run_callback(db, key, view, skip_missing);
    }
    using packed_operation =
        std::function<bool(const compact_zset::view&, db::batch_entry&)>;
    bool run_callback(shared_db& db, const std::string& key,
        const operation& callback, bool skip_missing,
        const packed_operation& packed_callback = {});
    bool query_range(shared_db& db, const std::string& key,
        const zset_query& query, std::vector<zset_member>* out, long long& count);
    bool open(shared_db& db, page_transaction& tx, const std::string& key,
        zset_store& store, long long now);
    bool import(zset_store& store);
    bool finish(zset_store& store);
    bool result(db::batch_status status);
    template<class Member>
    bool add_values(shared_db& db, const std::string& key,
        const Member* values, size_t count, const zadd_options& options,
        zadd_result& out);
    template<class Member>
    bool add_compact_values(page_transaction& tx, const std::string& key,
        const Member* values, size_t count, const zadd_options& options,
        zadd_result& out, int64_t deadline, bool existing);
    bool change(zset_store& store, const std::string& member, double input,
        const zadd_options& options, zadd_result& out);
    bool erase(zset_store& store, const std::vector<zset_member>& values);
    bool pop(zset_store& store, long long count, bool maximum,
        std::vector<zset_member>& out);
    static void erase_compact(const compact_zset::view& packed,
        db::batch_entry& root, const compact_zset::erase_mask& erased, unsigned count);
    bool store_compact(page_transaction& tx, const std::string& key,
        std::vector<std::pair<std::string, double>> values, bool& stored);
    static void pop_compact(const compact_zset::view& packed,
        db::batch_entry& root, long long count, bool maximum,
        std::vector<zset_member>& out);
    struct algebra_source;
    using source_map =
        std::map<std::string, std::unique_ptr<algebra_source>>;
    bool sources(shared_db& db, page_transaction& tx,
        const std::vector<std::string>& keys, source_map& out);
    bool finish(source_map& sources);
    bool source_error(const source_map& sources);
};

} // namespace dao
} // namespace pkv
