#include "stdafx.h"
#include "db/zset_store.h"
#include "db/mdb/mdb.h"
#include "coder/json_record_codec.h"
#include <cstdlib>
#include <iostream>
#include <new>
#include <random>

static thread_local long allocation_budget = -1;
void* operator new(std::size_t size) {
    if (allocation_budget == 0) throw std::bad_alloc();
    if (allocation_budget > 0) --allocation_budget;
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
#if __cplusplus >= 201402L
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
#endif

using namespace pkv;
using values = std::map<std::string, double>;
static bool use_tbb = false;
static const long long deadline = 4102444800123LL;
static void require(bool ok, const char* reason) {
    if (!ok) { std::cerr << reason << '\n'; std::exit(1); }
}
static shared_db memory() {
    shared_db database;
#if defined(HAS_TBB)
    if (use_tbb) database = db::create_mdb_tbb(4);
#endif
    if (!database) database = db::create_mdb(4);
    require(database->open(""), "memory open");
    return database;
}
static bool operation(shared_db database, const std::string& key,
    const std::function<bool(zset_store&)>& callback) {
    return database->transact_pages({key}, [&](page_transaction& tx) {
        zset_store store(tx, key);
        return store.open() && callback(store) && store.finish();
    }) == db::batch_status::ok;
}
static void seed(shared_db database, const std::string& key,
    const values& expected) {
    require(operation(database, key, [&](zset_store& store) {
        store.initialize(deadline);
        for (const auto& item : expected) {
            bool added, changed;
            if (!store.put(item.first, item.second, added, changed))
                return false;
        }
        return true;
    }), "seed");
}
static values contents(shared_db database, const std::string& key) {
    values out;
    require(operation(database, key, [&](zset_store& store) {
        return store.visit([&](const std::string& member, double score) {
            out.emplace(member, score); return true;
        });
    }), "contents");
    return out;
}
static bool same(const std::vector<zset_member>& a,
    const std::vector<zset_member>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].member != b[i].member || a[i].score != b[i].score)
            return false;
    return true;
}

static void test_ranges() {
    const std::vector<std::string> names = {"", "a", "aa", "b",
        std::string("a\0", 2), std::string("a\xff", 2),
        std::string(40000, 'x'), std::string(1, '\xff')};
    for (bool mixed : {false, true}) {
        auto database = memory();
        values expected;
        for (size_t i = 0; i < names.size(); ++i)
            expected[names[i]] = mixed ? static_cast<double>(i) - 3 : 7;
        expected["infinite-low"] = mixed ? -INFINITY : 7;
        expected["infinite-high"] = mixed ? INFINITY : 7;
        seed(database, "zset", expected);
        value_record record;
        record.header.kind = value_kind::zset; record.zset_value = expected;
        json_record_codec codec;
        std::string bytes;
        std::unique_ptr<record_view> reference;
        require(codec.encode(record, bytes) == codec_status::ok &&
            codec.decode(bytes, reference) == codec_status::ok, "reference");
        std::mt19937 random(92741);
        for (unsigned step = 0; step < 1200; ++step) {
            zset_query query;
            query.mode = static_cast<zrange_mode>(random() % 3);
            query.reverse = random() % 2;
            query.start = static_cast<int>(random() % 30) - 15;
            query.end = static_cast<int>(random() % 30) - 15;
            query.offset = static_cast<int>(random() % 15) - 2;
            query.limit = static_cast<int>(random() % 15) - 2;
            query.min_score.value = static_cast<int>(random() % 20) - 5;
            query.max_score.value = static_cast<int>(random() % 20) - 5;
            query.min_score.exclusive = random() % 2;
            query.max_score.exclusive = random() % 2;
            if (step % 7 == 0) query.min_score.value = -INFINITY;
            if (step % 11 == 0) query.max_score.value = INFINITY;
            query.min_lex.value = names[random() % names.size()];
            query.max_lex.value = names[random() % names.size()];
            query.min_lex.exclusive = random() % 2;
            query.max_lex.exclusive = random() % 2;
            if (step % 7 == 0) query.min_lex.infinity = -1;
            if (step % 11 == 0) query.max_lex.infinity = 1;
            std::vector<zset_member> actual, wanted;
            long long count, wanted_count;
            require(reference->zset_select(query, wanted) &&
                reference->zset_count(query, wanted_count), "reference query");
            require(operation(database, "zset", [&](zset_store& store) {
                return store.range(query, actual) && store.count(query, count);
            }) && same(actual, wanted) && count == wanted_count &&
                count == static_cast<long long>(wanted.size()), "range model");
        }
    }
}

static db::batch_status clone(shared_db source, const std::string& key,
    shared_db target, const std::string& destination) {
    return source->copy_pages_to(key, *target, destination,
        [&](page_transaction& from_tx, page_transaction& to_tx) {
        zset_store from(from_tx, key), to(to_tx, destination);
        bool copied;
        return from.open() && from.clone_to(to, copied) && copied &&
            to.finish();
    });
}

static void test_clone() {
    auto source = memory(), destination = memory();
    const values expected = {{"", -INFINITY}, {"a", 2}, {"b", 2},
        {std::string("a\0b", 3), 0}, {std::string(1024 * 1024, 'x'), INFINITY}};
    seed(source, "source", expected);
    seed(destination, "target", {{"old", 8}});
    require(clone(source, "source", destination, "target") ==
        db::batch_status::ok && contents(destination, "target") == expected,
        "independent cross-DB clone");
    require(operation(destination, "target", [&](zset_store& store) {
        require(store.metadata().expires_at_ms == deadline, "clone TTL");
        bool removed;
        return store.erase("a", removed) && removed;
    }) && contents(source, "source") == expected, "clone aliased source pages");
    require(destination->set("opaque", "old untyped bytes") &&
        clone(source, "source", destination, "opaque") ==
            db::batch_status::ok && contents(destination, "opaque") == expected,
        "clone could not replace another destination representation");
    auto retained = expected; retained.erase("a");
    require(source->copy_pages_to("missing", *destination, "target",
        [&](page_transaction& from_tx, page_transaction& to_tx) {
        zset_store from(from_tx, "missing"), to(to_tx, "target");
        bool copied = true;
        return from.open() && from.clone_to(to, copied) && !copied;
    }) == db::batch_status::ok &&
        contents(destination, "target") == retained,
        "missing source changed destination");
    require(clone(source, "source", source, "copy") == db::batch_status::ok &&
        contents(source, "copy") == expected, "same-DB clone");
    require(source->transact_pages({"source"}, [&](page_transaction& tx) {
        zset_store from(tx, "source"), to(tx, "source");
        bool copied;
        require(from.open() && !from.clone_to(to, copied) && !copied,
            "self clone accepted");
        return true;
    }) == db::batch_status::aborted, "ignored self-clone error committed");
    seed(source, "dirty", {{"before", 1}});
    require(source->transact_pages({"dirty", "dirty-copy"},
        [&](page_transaction& tx) {
        zset_store from(tx, "dirty"), to(tx, "dirty-copy");
        bool added, changed, copied;
        return from.open() && from.put("after", 4, added, changed) &&
            from.clone_to(to, copied) && copied && from.finish() && to.finish();
    }) == db::batch_status::ok && contents(source, "dirty-copy") ==
        values({{"before", 1}, {"after", 4}}), "clone ignored staged edits");
}

static void test_clone_faults() {
    bool failed = false, success = false;
    for (long budget = 0; budget < 3000 && !success; ++budget) {
        auto source = memory(), destination = memory();
        const values expected = {{"a", -1}, {"b", 2},
            {std::string(40000, 'x'), 3}};
        const values previous = {{"old", 4}};
        seed(source, "source", expected); seed(destination, "target", previous);
        std::string before, after;
        require(destination->get("target", before), "target root");
        db::batch_status status = db::batch_status::error;
        allocation_budget = budget;
        try { status = clone(source, "source", destination, "target"); }
        catch (const std::bad_alloc&) {}
        allocation_budget = -1;
        if (status == db::batch_status::ok) {
            success = true;
            require(contents(destination, "target") == expected,
                "successful clone incomplete");
        } else {
            failed = true;
            require(destination->get("target", after) && after == before &&
                contents(destination, "target") == previous &&
                contents(source, "source") == expected,
                "failed clone changed source/destination");
        }
    }
    require(failed && success, "incomplete clone fault sweep");
}

class tracking_db : public mdb {
public:
    tracking_db() : mdb(4) {}
    size_t reads = 0, blobs = 0;
protected:
    lookup_status dblookup_page(const std::string& key, uint64_t id,
        std::string& bytes) override {
        ++reads;
        const auto status = mdb::dblookup_page(key, id, bytes);
        if (status == lookup_status::found && bytes.compare(0, 3, "PMB") == 0)
            ++blobs;
        return status;
    }
};

static void test_locality() {
    auto tracked = std::make_shared<tracking_db>();
    require(tracked->open(""), "tracking open");
    shared_db database = tracked;
    values expected;
    for (unsigned i = 0; i < 10000; ++i)
        expected["member:" + std::to_string(i)] = i / 10;
    seed(database, "zset", expected);
    zset_query query;
    query.mode = zrange_mode::score;
    query.min_score.value = 510; query.max_score.value = 520;
    tracked->reads = 0;
    require(operation(database, "zset", [&](zset_store& store) {
        long long count;
        return store.count(query, count) && count == 110;
    }) && tracked->reads < 150, "score count traversed population");
    query.offset = 109; query.limit = 1;
    tracked->reads = 0;
    require(operation(database, "zset", [&](zset_store& store) {
        std::vector<zset_member> out;
        return store.range(query, out) && out.size() == 1 &&
            out[0].score == 520;
    }) && tracked->reads < 250, "score LIMIT/OFFSET traversed skipped members");
    query = zset_query(); query.start = 9900; query.end = 9901;
    query.reverse = true;
    tracked->reads = 0;
    require(operation(database, "zset", [&](zset_store& store) {
        std::vector<zset_member> out;
        return store.range(query, out) && out.size() == 2;
    }) && tracked->reads < 300, "rank seek traversed skipped members");
    for (auto& item : expected) item.second = 7;
    seed(database, "uniform", expected);
    query = zset_query(); query.mode = zrange_mode::lex;
    query.min_lex.value = "member:4000"; query.max_lex.value = "member:4010";
    tracked->reads = 0;
    require(operation(database, "uniform", [&](zset_store& store) {
        long long count;
        return store.count(query, count) && count > 0;
    }) && tracked->reads < 300, "uniform lex count traversed population");
}

static void test_corrupt_clone() {
    auto source = memory(), destination = memory();
    seed(source, "source", {{"only", 1}});
    seed(destination, "target", {{"old", -1}});
    require(source->transact_pages({"source"}, [&](page_transaction& tx) {
        zset_root root;
        auto& entry = *tx.roots().find("source");
        if (zset_page_codec::decode_root(entry.value, root) !=
            map_page_status::ok) return false;
        std::string bytes;
        map_node node;
        if (tx.lookup("source", root.members, bytes) != db::lookup_status::found
            || map_page_codec::decode_node(bytes, node) != map_page_status::ok)
            return false;
        if (!zset_page_codec::encode_score(2, node.value.inline_value))
            return false;
        return map_page_codec::encode_node(node, bytes) == map_page_status::ok
            && tx.put("source", node.id, bytes);
    }) == db::batch_status::ok, "inconsistent index fixture");
    std::string before, after;
    require(destination->get("target", before), "target root");
    require(clone(source, "source", destination, "target") !=
        db::batch_status::ok && destination->get("target", after) &&
        after == before && contents(destination, "target") ==
            values({{"old", -1}}), "inconsistent clone changed destination");
}

static void test_batched_range() {
    auto database = memory();
    const std::string key = "batched";
    values expected;
    for (unsigned i=0;i<512;++i) {
        auto member=std::to_string(i);
        if (i%17==0) member.append(100, '\0');
        expected.emplace(std::move(member),i%37==0 ? -0.0 : i%23);
    }
    seed(database,key,expected);
    for (bool reverse : {false,true}) {
        zset_query query;
        query.start=2; query.end=166; query.reverse=reverse;
        std::vector<zset_member> reference;
        select_zset(expected,query,reference);
        require(operation(database,key,[&](zset_store& store) {
            std::vector<zset_member> actual;
            if (!store.range(query,actual) || !same(reference,actual)) return false;
            for (size_t i=0;i<actual.size();++i)
                if (std::signbit(actual[i].score)!=std::signbit(reference[i].score))
                    return false;
            return true;
        }),"batched range, fragments, ties and signed zero");
    }
    std::string member;
    require(database->transact_page(key,[&](page_transaction& tx) {
        zset_root root;
        std::string bytes;
        map_node node;
        if (zset_page_codec::decode_root(tx.roots().find(key)->value,root)!=map_page_status::ok ||
            tx.lookup(key,root.members,bytes)!=db::lookup_status::found ||
            map_page_codec::decode_node(bytes,node)!=map_page_status::ok ||
            node.key.head) return false;
        member=node.key.inline_value;
        return zset_page_codec::encode_value(-900,node.value.inline_value) &&
            map_page_codec::encode_node(node,bytes)==map_page_status::ok &&
            tx.put(key,root.members,std::move(bytes));
    })==db::batch_status::ok,"batched range corrupt member fixture");
    const auto ordered=ordered_zset(expected);
    const auto it=std::find_if(ordered.begin(),ordered.end(),[&](const zset_member& item) {
        return item.member==member;
    });
    require(it!=ordered.end(),"corrupt member belongs to set");
    for (const long long width : {4, 8, 32}) {
        zset_query query;
        query.start = std::min<long long>(it - ordered.begin(),
            static_cast<long long>(ordered.size()) - width);
        query.end = query.start + width - 1;
        require(!operation(database, key, [&](zset_store& store) {
            std::vector<zset_member> out = {{"sentinel", 7}};
            require(!store.range(query, out) && out.empty() &&
                store.error() == zset_store::status::invalid,
                "batch rejects inconsistent score");
            return true;
        }), "batch corruption abort");
    }
}

static void test_score_range_order() {
    auto database = memory();
    const std::string key = "misordered";
    values expected;
    for (unsigned i = 0; i < 128; ++i)
        expected.emplace(std::to_string(i), i);
    seed(database, key, expected);
    double score = 0;
    require(database->transact_page(key, [&](page_transaction& tx) {
        zset_root root;
        std::string bytes, left_bytes;
        map_node node, left;
        if (zset_page_codec::decode_root(tx.roots().find(key)->value, root) !=
                map_page_status::ok ||
            tx.lookup(key, root.scores, bytes) != db::lookup_status::found ||
            map_page_codec::decode_node(bytes, node) != map_page_status::ok ||
            !node.left || node.key.head ||
            tx.lookup(key, node.left, left_bytes) != db::lookup_status::found ||
            map_page_codec::decode_node(left_bytes, left) != map_page_status::ok ||
            left.key.head ||
            !zset_page_codec::decode_score_prefix(left.key.inline_value, score))
            return false;
        // Both keys still match the member index, but belong to opposite
        // sides of the root's search bound.
        std::swap(node.key, left.key);
        return map_page_codec::encode_node(node, bytes) == map_page_status::ok &&
            map_page_codec::encode_node(left, left_bytes) == map_page_status::ok &&
            tx.put(key, node.id, std::move(bytes)) &&
            tx.put(key, left.id, std::move(left_bytes));
    }) == db::batch_status::ok, "swap score keys without changing tree counts");
    for (bool reverse : {false, true}) {
        zset_query query;
        query.start = std::min<long long>(static_cast<long long>(score), 120);
        query.end = query.start + 7;
        query.reverse = reverse;
        if (reverse) {
            const auto first = 127 - query.end;
            query.end = 127 - query.start;
            query.start = first;
        }
        require(!operation(database, key, [&](zset_store& store) {
            std::vector<zset_member> out;
            require(!store.range(query, out) && out.empty() &&
                store.error() == zset_store::status::invalid,
                "range accepted a score key outside its ancestor bound");
            return true;
        }), "misordered range corruption abort");
    }
}

static void test_extreme_ranges() {
    auto database = memory();
    const values expected = {{"", -INFINITY}, {"a", 0}, {"b", INFINITY}};
    seed(database, "zset", expected);
    for (bool reverse : {false, true}) {
        zset_query query;
        query.reverse = reverse; query.start = LLONG_MIN; query.end = LLONG_MAX;
        require(operation(database, "zset", [&](zset_store& store) {
            std::vector<zset_member> out;
            long long count;
            return store.range(query, out) && out.size() == 3 &&
                store.count(query, count) && count == 3;
        }), "extreme rank endpoints");
        query.mode = zrange_mode::score;
        query.min_score.value = -INFINITY; query.max_score.value = INFINITY;
        query.offset = 1; query.limit = LLONG_MAX;
        require(operation(database, "zset", [&](zset_store& store) {
            std::vector<zset_member> out;
            long long count;
            return store.range(query, out) && out.size() == 2 &&
                store.count(query, count) && count == 2;
        }), "extreme score LIMIT");
        query.mode = zrange_mode::lex;
        query.min_lex.infinity = -1; query.max_lex.infinity = 1;
        query.offset = LLONG_MAX;
        require(operation(database, "zset", [&](zset_store& store) {
            std::vector<zset_member> out;
            long long count;
            return store.range(query, out) && out.empty() &&
                store.count(query, count) && count == 0;
        }), "extreme lex OFFSET");
    }
    std::string before, after;
    require(database->get("zset", before), "NaN query root");
    require(!operation(database, "zset", [&](zset_store& store) {
        zset_query query; query.mode = zrange_mode::score;
        query.min_score.value = NAN;
        std::vector<zset_member> out = {{"sentinel", 13}};
        require(!store.range(query, out) && out.empty() && store.error() ==
            zset_store::status::invalid, "NaN bound was accepted");
        return true;
    }) && database->get("zset", after) && before == after,
        "invalid query changed data");
}

int main() {
    acl::acl_cpp_init();
    test_ranges(); test_clone(); test_clone_faults(); test_locality();
    test_corrupt_clone(); test_extreme_ranges(); test_batched_range();
    test_score_range_order();
#if defined(HAS_TBB)
    use_tbb = true;
    test_ranges(); test_clone(); test_clone_faults();
    test_corrupt_clone(); test_extreme_ranges(); test_batched_range();
    test_score_range_order();
#endif
    std::cout << "Native ZSet range seeks and independent transfers passed\n";
}
