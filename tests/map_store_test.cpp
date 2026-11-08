#include "stdafx.h"
#include "db/map_store.h"
#include "db/mdb/mdb.h"
#include "db/node_write_workspace.h"
#include "db/node_key_workspace.h"
#include <algorithm>
#include <array>
#include <climits>
#include <cstdlib>
#include <iostream>
#include <random>
#include <memory>
#include <thread>
#include <set>
#include <new>

static thread_local long allocation_budget = -1;
void* operator new(std::size_t size) {
    if (allocation_budget == 0) throw std::bad_alloc();
    if (allocation_budget > 0) --allocation_budget;
    if (void* pointer = std::malloc(size ? size : 1)) return pointer;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
#if __cplusplus >= 201402L
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer);
    }
void operator delete[](void* pointer, std::size_t) noexcept {
    std::free(pointer);
}
#endif

using namespace pkv;
using fields = std::map<std::string, std::string>;
static const long long expiry = 4102444800123LL;

static void require(bool ok, const char* reason) {
    if (!ok) { std::cerr << reason << '\n'; std::exit(1); }
}

class tracking_db : public mdb {
public:
    tracking_db() : mdb(4) {}
    size_t reads = 0, blobs = 0, writes = 0, largest = 0;
protected:
    lookup_status dblookup_page(const std::string& owner, uint64_t id,
        std::string& out) override {
        ++reads;
        const auto result = mdb::dblookup_page(owner, id, out);
        if (result == lookup_status::found && out.compare(0, 3, "PMB") == 0)
            ++blobs;
        return result;
    }
    batch_status dbapply_pages(const std::vector<batch_entry>& roots,
        const std::vector<page_change>& pages) override {
        writes += pages.size();
        for (const auto& page : pages) largest = std::max(largest,
            page.value.size());
        return mdb::dbapply_pages(roots, pages);
    }
};

static std::shared_ptr<tracking_db> memory() {
    auto database = std::make_shared<tracking_db>();
    require(database->open(""), "memory open");
    return database;
}

static bool operation(shared_db database, const std::string& key,
    const std::function<bool(map_store&)>& callback,
    map_kind kind = map_kind::hash) {
    return database->transact_pages({key}, [&](page_transaction& tx) {
        map_store map(tx, key, kind, false);
        const bool ok = map.open() && callback(map) && map.finish();
        if (!ok) std::cerr << "map error " << static_cast<int>(map.error()) <<
            " elements " << map.metadata().elements << '\n';
        return ok;
    }) == db::batch_status::ok;
}

static void seed(shared_db database, const fields& expected) {
    require(operation(database, "hash", [&](map_store& map) {
        map.initialize(expiry);
        for (const auto& item : expected) {
            bool added;
            if (!map.put(item.first, item.second, false, added) || !added)
                return false;
        }
        return true;
    }), "seed");
}

struct audit {
    page_transaction& tx;
    std::set<uint64_t> visited;
    fields actual;
    std::string previous;
    bool has_previous = false;
    unsigned previous_bucket = 0;
    explicit audit(page_transaction& view) : tx(view) {}
    std::string blob(const map_blob& value) {
        if (!value.head) return value.inline_value;
        std::string result;
        uint64_t id = value.head;
        while (id) {
            require(visited.insert(id).second, "shared blob/cycle");
            std::string bytes;
            map_fragment page;
            require(tx.lookup("hash", id, bytes) == db::lookup_status::found &&
                map_page_codec::decode_fragment(bytes, page) ==
                    map_page_status::ok && page.id == id && page.offset ==
                    result.size(), "blob chain");
            result += page.bytes;
            id = page.next;
        }
        require(result.size() == value.length, "blob length");
        return result;
    }
    std::pair<unsigned, uint64_t> node(uint64_t id, unsigned depth = 0) {
        if (!id) return {0, 0};
        require(depth < 128 && visited.insert(id).second, "tree cycle/alias");
        std::string bytes;
        map_node value;
        require(tx.lookup("hash", id, bytes) == db::lookup_status::found &&
            bytes.size() <= 16384 && map_page_codec::decode_node(bytes,
                value) == map_page_status::ok && value.id == id, "node page");
        const auto before = node(value.left, depth + 1);
        const auto key = blob(value.key);
        const auto bucket = scan_bucket(key);
        require(bucket == value.bucket && (!has_previous ||
            previous_bucket < bucket || (previous_bucket == bucket &&
                previous < key)), "BST bucket ordering");
        previous = key; previous_bucket = static_cast<unsigned>(bucket);
        has_previous = true;
        require(actual.emplace(key, blob(value.value)).second, "duplicate key");
        const auto after = node(value.right, depth + 1);
        require(value.elements == before.second + after.second + 1 &&
            value.height == std::max(before.first, after.first) + 1 &&
            std::abs(static_cast<int>(before.first) -
                static_cast<int>(after.first)) <= 1, "AVL invariant");
        return {value.height, value.elements};
    }
};

static void verify(shared_db database, const fields& expected,
    bool pages = false) {
    require(database->transact_pages({"hash"}, [&](page_transaction& tx) {
        auto& entry = *tx.roots().find("hash");
        if (!entry.found) return expected.empty();
        map_root root;
        require(map_page_codec::decode_root(
            entry.value, root) == map_page_status::ok &&
            root.expires_at_ms == expiry, "root metadata");
        audit inspection(tx);
        const auto counts = inspection.node(root.tree);
        require(counts.second == root.elements && inspection.actual == expected,
            "tree differs from model");
        if (pages) {
            for (uint64_t id = 1; id <= root.last_id; ++id) {
                std::string bytes;
                const bool found = tx.lookup("hash", id, bytes) ==
                    db::lookup_status::found;
                require(found == (inspection.visited.count(id) != 0),
                    "orphaned physical page");
            }
        }
        return true;
    }) == db::batch_status::ok, "verify");
}

static void test_codec() {
    map_root root;
    root.tree = 1; root.last_id = 3; root.elements = 1;
    root.expires_at_ms = expiry;
    std::string bytes, output = "sentinel";
    require(map_page_codec::encode_root(root, bytes) == map_page_status::ok &&
        bytes.size() == 20 && bytes.substr(0, 4) == std::string("PMR\4", 4),
        "root wire");
    for (size_t length = 0; length < bytes.size(); ++length) {
        map_root out; out.last_id = 99;
        require(map_page_codec::decode_root(bytes.substr(0, length), out) !=
            map_page_status::ok && out.last_id == 99, "truncated root");
    }
    bytes[3] = 127;
    require(map_page_codec::decode_root(bytes, root) ==
        map_page_status::unsupported, "future root");
    bytes[3] = 1;
    require(map_page_codec::decode_root(bytes, root) ==
        map_page_status::unsupported,
        "old lexical prototype was reinterpreted");
    root.expires_at_ms = -1;
    root.last_id = UINT16_MAX;
    map_root decoded;
    require(map_page_codec::encode_root(root, bytes) == map_page_status::ok &&
        bytes.size() == 12 && map_page_codec::decode_root(bytes, decoded) ==
        map_page_status::ok && decoded.last_id == UINT16_MAX &&
        decoded.expires_at_ms == -1, "compact root counter limit");
    ++root.last_id;
    require(map_page_codec::encode_root(root, bytes) == map_page_status::ok &&
        bytes.size() == 48 && bytes[3] == 2 &&
        map_page_codec::decode_root(bytes, decoded) == map_page_status::ok &&
        decoded.last_id == root.last_id, "wide root counter");
    map_node node;
    node.id = 1; node.key.inline_value = std::string("a\0", 2);
    node.key.length = 2;
    node.value.inline_value.assign(128, 'v'); node.value.length = 128;
    require(map_page_codec::encode_node(node, bytes) == map_page_status::ok,
        "node encode");
    for (size_t length = 0; length < bytes.size(); ++length) {
        map_node out; out.id = 99;
        require(map_page_codec::decode_node(bytes.substr(0, length), out) !=
            map_page_status::ok && out.id == 99, "truncated node");
    }
    map_node out;
    require(map_page_codec::decode_node(bytes, out) == map_page_status::ok &&
        out.key.inline_value == node.key.inline_value &&
        out.value.inline_value == node.value.inline_value, "node binary");
    node.id = uint64_t(UINT16_MAX) + 1;
    require(map_page_codec::encode_packed_node(node, bytes) == map_page_status::ok &&
        bytes.size() == 19 + 2 + 128 && bytes[3] == 15, "24-bit packed node size");
    for (size_t length = 0; length < bytes.size(); ++length) {
        map_node truncated; truncated.id = 99;
        require(map_page_codec::decode_node(bytes.substr(0, length), truncated) !=
            map_page_status::ok && truncated.id == 99, "truncated packed node");
    }
    map_node_view view;
    require(map_page_codec::decode_node(bytes, out) == map_page_status::ok &&
        map_page_codec::decode_node_view(bytes, view) == map_page_status::ok &&
        out.key.inline_value == node.key.inline_value &&
        out.value.inline_value == node.value.inline_value &&
        std::string(view.key.data, view.key.inline_size()) == node.key.inline_value &&
        std::string(view.value.data, view.value.inline_size()) == node.value.inline_value,
        "packed node binary and borrowed view");
    auto damaged = bytes;
    damaged[5] = 0;
    require(map_page_codec::decode_node(damaged, out) == map_page_status::invalid &&
        map_page_codec::decode_node_view(damaged, view) == map_page_status::invalid,
        "invalid packed height");
    node.id = 1;
    node.value.inline_value.assign(9, 'v'); node.value.length = 9;
    require(map_page_codec::encode_packed_node(node, bytes) == map_page_status::ok &&
        bytes.size() == 22 && bytes[3] == 4, "compact leaf size");
    require(map_page_codec::decode_node(bytes, out) == map_page_status::ok &&
        map_page_codec::decode_node_view(bytes, view) == map_page_status::ok &&
        out.id == node.id && out.height == 1 && out.elements == 1 &&
        !out.left && !out.right &&
        out.key.inline_value == node.key.inline_value &&
        out.value.inline_value == node.value.inline_value &&
        std::string(view.value.data, view.value.inline_size()) == node.value.inline_value,
        "compact leaf round trip");
    bytes[6] = 10;
    require(map_page_codec::decode_node(bytes, out) == map_page_status::invalid,
        "compact leaf length mismatch");
    node.left = 2; node.right = 3; node.height = 2; node.elements = 3;
    require(map_page_codec::encode_packed_node(node, bytes) == map_page_status::ok &&
        bytes.size() == 22 && bytes[3] == 5, "compact branch size");
    require(map_page_codec::decode_node(bytes, out) == map_page_status::ok &&
        map_page_codec::decode_node_view(bytes, view) == map_page_status::ok &&
        out.id == node.id && out.left == 2 && out.right == 3 &&
        out.height == 2 && out.elements == 3 &&
        out.key.inline_value == node.key.inline_value &&
        out.value.inline_value == node.value.inline_value &&
        view.left == 2 && view.right == 3 && view.elements == 3,
        "compact branch round trip");
    bytes[8] = bytes[7];
    require(map_page_codec::decode_node(bytes, out) == map_page_status::invalid &&
        map_page_codec::decode_node_view(bytes, view) == map_page_status::invalid,
        "compact branch rejects self link");
    node.value.inline_value.push_back('x'); node.value.length = 10;
    require(map_page_codec::encode_packed_node(node, bytes) == map_page_status::ok &&
        bytes.size() == 22 && bytes[3] == 6 &&
        map_page_codec::decode_node(bytes, out) == map_page_status::ok &&
        map_page_codec::decode_node_view(bytes, view) == map_page_status::ok &&
        out.left == 2 && out.right == 3 && out.elements == 3 &&
        out.key.inline_value == node.key.inline_value &&
        out.value.inline_value == node.value.inline_value &&
        std::string(view.value.data, view.value.inline_size()) == node.value.inline_value,
        "narrow branch round trip");
    bytes[5] = char(0xf2);
    require(map_page_codec::decode_node(bytes, out) == map_page_status::invalid &&
        map_page_codec::decode_node_view(bytes, view) == map_page_status::invalid,
        "narrow branch rejects invalid key length");
    node.left = UINT16_MAX;
    require(map_page_codec::encode_packed_node(node, bytes) == map_page_status::ok &&
        bytes[3] == 9 && bytes.size() == 16 + 12 &&
        map_page_codec::decode_node(bytes, out) == map_page_status::ok &&
        map_page_codec::decode_node_view(bytes, view) == map_page_status::ok &&
        out.left == node.left && view.left == node.left &&
        out.key.inline_value == node.key.inline_value &&
        out.value.inline_value == node.value.inline_value, "16-bit branch round trip");
    require(map_page_codec::decode_node_view(bytes.substr(0, bytes.size() - 1), view)
        == map_page_status::invalid, "16-bit branch rejects truncated payload");
    ++node.left;
    require(map_page_codec::encode_packed_node(node, bytes) == map_page_status::ok &&
        bytes[3] == 3 && map_page_codec::decode_node(bytes, out) == map_page_status::ok &&
        out.left == node.left, "large branch link uses 32-bit packed format");
    node.id = uint64_t(UINT32_MAX) + 1;
    require(map_page_codec::encode_packed_node(node, bytes) == map_page_status::ok &&
        bytes[3] == 2 && map_page_codec::decode_node(bytes, out) == map_page_status::ok &&
        out.id == node.id, "large node ID retains wide format");
    node.left = node.id;
    require(map_page_codec::encode_node(node, output) ==
        map_page_status::invalid && output == "sentinel", "invalid node");
    node.id = UINT16_MAX;
    node.left = node.right = 0;
    node.elements = node.height = 1;
    node.value.inline_value.assign(12, 'v'); node.value.length = 12;
    require(map_page_codec::encode_packed_node(node, bytes) == map_page_status::ok &&
        bytes.size() == 22 && bytes[3] == 7 &&
        map_page_codec::decode_node(bytes, out) == map_page_status::ok &&
        map_page_codec::decode_node_view(bytes, view) == map_page_status::ok &&
        out.id == node.id && out.height == 1 && out.elements == 1 &&
        !out.left && !out.right && out.key.inline_value == node.key.inline_value &&
        out.value.inline_value == node.value.inline_value && view.id == node.id &&
        std::string(view.value.data, view.value.inline_size()) == node.value.inline_value,
        "short leaf round trip");
    bytes[5] = 15;
    require(map_page_codec::decode_node(bytes, out) == map_page_status::invalid &&
        map_page_codec::decode_node_view(bytes, view) == map_page_status::invalid,
        "short leaf rejects invalid key length");
    ++node.id;
    require(map_page_codec::encode_packed_node(node, bytes) == map_page_status::ok &&
        bytes[3] == 3, "large leaf ID uses packed format");
    node.id = UINT8_MAX; node.left = 2; node.right = 3;
    node.height = 5; node.elements = 32;
    node.key.inline_value.assign(14, 'k'); node.key.length = 14;
    node.value.inline_value.clear(); node.value.length = 0;
    require(map_page_codec::encode_packed_node(node, bytes) == map_page_status::ok &&
        bytes.size() == 22 && bytes[3] == 8 &&
        map_page_codec::decode_node(bytes, out) == map_page_status::ok &&
        map_page_codec::decode_node_view(bytes, view) == map_page_status::ok &&
        out.id == node.id && out.left == 2 && out.right == 3 &&
        out.height == 5 && out.elements == 32 &&
        out.key.inline_value == node.key.inline_value && out.value.length == 0 &&
        view.height == 5 && view.elements == 32,
        "short branch round trip");
    bytes[6] = bytes[5];
    require(map_page_codec::decode_node(bytes, out) == map_page_status::invalid &&
        map_page_codec::decode_node_view(bytes, view) == map_page_status::invalid,
        "short branch rejects self link");
    ++node.elements;
    require(map_page_codec::encode_packed_node(node, bytes) == map_page_status::ok &&
        bytes[3] == 9, "large subtree uses 16-bit packed format");
    node.elements = 32;
    node.key.inline_value = std::string("k\0ey42", 6); node.key.length = 6;
    node.value.inline_value.assign(8, 's'); node.value.length = 8;
    node.bucket = static_cast<uint8_t>(scan_bucket(node.key.inline_value));
    require(map_page_codec::encode_packed_node(node, bytes, true) == map_page_status::ok &&
        bytes[3] == 13 && bytes.size() == 22 &&
        map_page_codec::decode_node(bytes, out) == map_page_status::ok &&
        map_page_codec::decode_node_view(bytes, view) == map_page_status::ok &&
        out.id == node.id && out.left == node.left && out.right == node.right &&
        out.bucket == node.bucket && view.bucket_derived && view.elements == 32 &&
        out.key.inline_value == node.key.inline_value &&
        out.value.inline_value == node.value.inline_value,
        "short valued branch round trip");
    require(map_page_codec::decode_node_view(bytes.substr(0, 21), view) ==
        map_page_status::invalid, "short valued branch rejects truncation");
    require(map_page_codec::encode_packed_node(node, bytes) == map_page_status::ok &&
        bytes[3] == 9, "short valued branch requires a derived bucket");
    node.value.inline_value.clear(); node.value.length = 0;
    node.id = 4095; node.left = 4094; node.right = 257;
    node.bucket = 255; node.height = 6; node.elements = 32;
    node.key.inline_value = std::string(19, 'k') + '\0'; node.key.length = 20;
    require(map_page_codec::encode_packed_node(node, bytes) == map_page_status::ok &&
        bytes[3] == 11 && bytes.size() == 31 &&
        map_page_codec::decode_node(bytes, out) == map_page_status::ok &&
        map_page_codec::decode_node_view(bytes, view) == map_page_status::ok &&
        out.id == node.id && out.left == node.left && out.right == node.right &&
        out.key.inline_value == node.key.inline_value && !out.value.length &&
        view.bucket == node.bucket && view.height == node.height &&
        view.elements == node.elements, "compact inline member round trip");
    bytes[6] = static_cast<char>(static_cast<unsigned char>(bytes[6]) | 0x10);
    require(map_page_codec::decode_node(bytes, out) == map_page_status::invalid &&
        map_page_codec::decode_node_view(bytes, view) == map_page_status::invalid,
        "compact inline member rejects reserved link bits");
    ++node.id;
    require(map_page_codec::encode_packed_node(node, bytes) == map_page_status::ok &&
        bytes[3] == 9 &&
        bytes.compare(8, 8, "\x10\x00\x0f\xfe\x01\x01\x00\x20", 8) == 0 &&
        map_page_codec::decode_node_view(bytes, view) == map_page_status::ok &&
        view.id == node.id && view.left == node.left &&
        view.right == node.right && view.elements == node.elements,
        "compact inline member preserves medium node wire format");
    node.left = node.right = 0; node.height = node.elements = 1;
    node.value.inline_value = std::string(19, 'v') + '\0'; node.value.length = 20;
    node.bucket = static_cast<uint8_t>(scan_bucket(node.key.inline_value));
    require(map_page_codec::encode_packed_node(node, bytes) == map_page_status::ok &&
        bytes[3] == 12 && bytes.size() == 47 &&
        map_page_codec::decode_node(bytes, out) == map_page_status::ok &&
        map_page_codec::decode_node_view(bytes, view) == map_page_status::ok &&
        view.bucket == node.bucket && view.id == node.id &&
        view.bucket_derived && out.bucket_derived &&
        out.key.inline_value == node.key.inline_value &&
        out.value.inline_value == node.value.inline_value,
        "compact Hash leaf round trip");
    bytes[6] = 40;
    require(map_page_codec::decode_node_view(bytes, view) == map_page_status::invalid,
        "compact Hash leaf rejects a missing value");
    require(map_page_codec::encode_node(node, bytes) == map_page_status::ok &&
        map_page_codec::decode_node_scratch(bytes, out) == map_page_status::ok &&
        !out.bucket_derived, "reused node must check stored buckets");
    node.id = UINT32_MAX;
    require(map_page_codec::encode_packed_node(node, bytes, true) == map_page_status::ok &&
        bytes[3] == 14 && bytes.size() == 49 &&
        map_page_codec::decode_node(bytes, out) == map_page_status::ok &&
        map_page_codec::decode_node_view(bytes, view) == map_page_status::ok &&
        out.id == node.id && out.bucket == node.bucket && view.bucket_derived &&
        out.key.inline_value == node.key.inline_value &&
        out.value.inline_value == node.value.inline_value,
        "wide hashed leaf round trip");
    bytes[8] = static_cast<char>(0x80 | 41);
    require(map_page_codec::decode_node_view(bytes, view) == map_page_status::invalid,
        "wide leaf rejects a key longer than its payload");
    node.value.inline_value.resize(3); node.value.length = 3;
    require(map_page_codec::encode_packed_node(node, bytes, true) == map_page_status::ok &&
        bytes[3] == 3, "short hashed payload keeps its packed encoding");
    auto score_leaf = node;
    score_leaf.key.inline_value.resize(12);
    score_leaf.key.length = 12;
    score_leaf.value.inline_value.resize(8);
    score_leaf.value.length = 8;
    score_leaf.bucket = static_cast<uint8_t>(scan_bucket(score_leaf.key.inline_value));
    bytes.reserve(96);
    const auto capacity = bytes.capacity();
    require(map_page_codec::encode_packed_node(score_leaf, bytes, true) == map_page_status::ok &&
        bytes[3] == 14 && bytes.size() == 29 && bytes.capacity() == capacity &&
        map_page_codec::decode_node(bytes, out) == map_page_status::ok &&
        out.id == score_leaf.id && out.bucket == score_leaf.bucket &&
        out.key.inline_value == score_leaf.key.inline_value &&
        out.value.inline_value == score_leaf.value.inline_value,
        "wide short score leaf reuses its encoding buffer");
    node.bucket = 0;
    node.value.inline_value.clear(); node.value.length = 0;
    require(map_page_codec::encode_packed_node(node, bytes) == map_page_status::ok &&
        bytes[3] == 14 && bytes.size() == 29 &&
        map_page_codec::decode_node(bytes, out) == map_page_status::ok &&
        map_page_codec::decode_node_view(bytes, view) == map_page_status::ok &&
        out.id == node.id && out.height == 1 && out.elements == 1 &&
        out.key.inline_value == node.key.inline_value && !view.value.length &&
        !view.bucket && !view.bucket_derived,
        "score leaf round trip");
    require(map_page_codec::decode_node_view(bytes.substr(0, 28), view) ==
        map_page_status::invalid, "score leaf rejects a truncated key");
    node.id = 0xffffff; node.left = 0xfffffe; node.right = 0x10000;
    node.height = 12; node.elements = UINT16_MAX;
    node.key.inline_value.assign(28, 'k'); node.key.length = 28;
    require(map_page_codec::encode_packed_node(node, bytes) == map_page_status::ok &&
        bytes[3] == 15 && bytes.size() == 47 &&
        map_page_codec::decode_node(bytes, out) == map_page_status::ok &&
        map_page_codec::decode_node_view(bytes, view) == map_page_status::ok &&
        out.id == node.id && out.left == node.left && out.right == node.right &&
        out.height == node.height && out.elements == node.elements &&
        view.id == node.id && view.left == node.left && view.right == node.right &&
        out.key.inline_value == node.key.inline_value,
        "24-bit links and 16-bit subtree count round trip");
    require(map_page_codec::decode_node_view(bytes.substr(0, 46), view) ==
        map_page_status::invalid, "24-bit node rejects truncated payload");
    bytes[11] = bytes[12] = bytes[13] = static_cast<char>(255);
    require(map_page_codec::decode_node_view(bytes, view) == map_page_status::invalid,
        "24-bit node rejects self link");
    ++node.id;
    require(map_page_codec::encode_packed_node(node, bytes) == map_page_status::ok &&
        bytes[3] == 3, "25-bit node keeps 32-bit fields");
    --node.id; ++node.elements;
    require(map_page_codec::encode_packed_node(node, bytes) == map_page_status::ok &&
        bytes[3] == 3, "17-bit subtree count keeps 32-bit fields");
    bytes[3] = 10;
    require(map_page_codec::decode_node(bytes, out) == map_page_status::unsupported &&
        map_page_codec::decode_node_view(bytes, view) == map_page_status::unsupported,
        "unknown node version with a valid signature");
    bytes[0] = 'X';
    bytes[3] = 14;
    require(map_page_codec::decode_node(bytes, out) == map_page_status::invalid &&
        map_page_codec::decode_node_view(bytes, view) == map_page_status::invalid,
        "known node version with an invalid signature");
    map_fragment fragment;
    fragment.id = 2; fragment.bytes.assign(16352, 'x');
    require(map_page_codec::encode_fragment(fragment, bytes) ==
        map_page_status::ok && bytes.size() == 16384, "fragment bound");
    fragment.bytes.push_back('x');
    require(map_page_codec::encode_fragment(fragment, output) ==
        map_page_status::invalid && output == "sentinel", "oversized fragment");
}

static void test_hashed_key_leaf() {
    map_node node;
    node.id = UINT16_MAX;
    node.height = 1;
    node.elements = 1;
    node.key.inline_value = std::string(12, 'm') + '\0' + "member!";
    node.key.length = node.key.inline_value.size();
    node.bucket = static_cast<uint8_t>(scan_bucket(node.key.inline_value));
    map_node decoded;
    map_node_view view;
    std::string bytes;
    require(map_page_codec::encode_packed_node(node, bytes, true) ==
        map_page_status::ok && bytes[3] == 16 && bytes.size() == 27 &&
        map_page_codec::decode_node(bytes, decoded) == map_page_status::ok &&
        map_page_codec::decode_node_view(bytes, view) == map_page_status::ok &&
        decoded.id == node.id && decoded.bucket == node.bucket &&
        decoded.key.inline_value == node.key.inline_value &&
        view.height == 1 && view.elements == 1 && !view.value.length &&
        !view.left && !view.right && !view.bucket_derived,
        "compact hashed key-only leaf round trip");
    require(map_page_codec::decode_node_view(bytes.substr(0, 21), view) ==
        map_page_status::invalid, "compact hashed leaf rejects short keys");
    bytes.resize(32);
    require(map_page_codec::decode_node_view(bytes, view) ==
        map_page_status::invalid, "compact hashed leaf rejects long keys");
    bytes.resize(27);
    bytes[4] = bytes[5] = 0;
    require(map_page_codec::decode_node_view(bytes, view) ==
        map_page_status::invalid, "compact hashed leaf rejects zero page ID");
    ++node.id;
    require(map_page_codec::encode_packed_node(node, bytes, true) ==
        map_page_status::ok && bytes[3] == 3 &&
        map_page_codec::decode_node(bytes, decoded) == map_page_status::ok &&
        decoded.id == node.id, "wide page ID keeps existing encoding");
}

static void test_model() {
    auto database = memory();
    fields expected;
    seed(database, expected);
    std::mt19937 random(7191);
    for (unsigned step = 0; step < 1600; ++step) {
        auto key = std::to_string(random() % 100);
        if (step % 7 == 0) key = std::string("\0\xff", 2) + key;
        if (step % 11 == 0) key.clear();
        const auto value = random() % 7 ? std::to_string(random()) :
            std::string(20000, static_cast<char>(random()));
        const bool nx = random() % 3 == 0;
        const auto choice = random() % 4;
        const bool accepted = operation(database, "hash", [&](map_store& map) {
            if (!map.exists()) map.initialize(expiry);
            bool found;
            if (choice == 0) {
                if (!map.erase(key, found)) return false;
                require(found == (expected.erase(key) != 0), "erase count");
            } else if (choice == 1) {
                std::string out = "sentinel";
                if (!map.get(key, out, found)) return false;
                require(found == (expected.count(key) != 0) &&
                    (!found || out == expected.at(key)), "get model");
            } else {
                const bool existed = expected.count(key) != 0;
                if (!map.put(key, value, nx, found)) return false;
                require(found == !existed, "put count");
                if (!nx || !existed) expected[key] = value;
            }
            require(map.metadata().elements == expected.size(), "map count");
            return true;
        });
        if (!accepted) std::cerr << "model step " << step << " operation " <<
            choice << " key " << key << '\n';
        require(accepted, "model operation");
        if (step % 40 == 0) verify(database, expected);
    }
    verify(database, expected, true);
    require(database->largest <= 16384, "physical value exceeded page bound");
}

static void test_value_fragment_reuse() {
    auto database = memory();
    uint64_t last_id = 0;
    require(operation(database, "hash", [&](map_store& map) {
        map.initialize(expiry);
        bool added;
        if (!map.put("field", std::string(20000, 'a'), false, added) || !added)
            return false;
        last_id = map.metadata().last_id;
        return true;
    }), "seed fragmented Hash value");
    const std::string multi_page(20000, 'x');
    database->writes = 0;
    require(operation(database, "hash", [&](map_store& map) {
        bool added;
        return map.put("field", multi_page, false, added) && !added &&
            map.metadata().last_id == last_id;
    }) && database->writes == 2, "equal-length replacement reuses the full fragment chain");
    verify(database, {{"field", multi_page}}, true);
    for (const auto length : {size_t(256), size_t(512), size_t(16)}) {
        const std::string value(length, 'b');
        require(operation(database, "hash", [&](map_store& map) {
            bool added;
            return map.put("field", value, false, added) && !added &&
                map.metadata().last_id == last_id;
        }), "replace fragmented Hash value without growing page ID");
        verify(database, {{"field", value}}, true);
    }
    require(operation(database, "hash", [&](map_store& map) {
        bool added;
        return map.put("field", std::string(256, 'c'), false, added) && !added;
    }), "restore fragmented value");
    database->writes = 0;
    const std::string replacement(256, 'd');
    require(operation(database, "hash", [&](map_store& map) {
        bool added;
        return map.put("field", replacement, false, added) && !added;
    }) && database->writes == 1, "equal-length replacement only writes the fragment");
    verify(database, {{"field", replacement}}, true);
}

static void test_insert_rotations() {
    std::vector<std::string> keys;
    for (unsigned i = 0; keys.size() < 24; ++i) {
        const auto key = "rotation:" + std::to_string(i);
        if (scan_bucket(key) == 0) keys.push_back(key);
    }
    std::sort(keys.begin(), keys.end());
    const unsigned cases[][3] = {{2, 1, 0}, {0, 1, 2}, {2, 0, 1}, {0, 2, 1}};
    for (const auto& sequence : cases) {
        auto database = memory();
        fields expected;
        seed(database, expected);
        const auto insert = [&](unsigned index) {
            const auto& key = keys[index];
            const auto value = std::string(index % 2 ? 20000 : 128, 'v');
            require(operation(database, "hash", [&](map_store& map) {
                if (!map.exists()) map.initialize(expiry);
                bool added;
                return map.put(key, value, false, added) && added;
            }), "rotation insert");
            expected[key] = value;
            // This audit decodes the stored tree independently, checking
            // every link, AVL height and subtree element count.
            verify(database, expected, true);
        };
        for (unsigned index : sequence) insert(index);
        for (unsigned i = 3; i < keys.size(); ++i) insert(i);
        require(operation(database, "hash", [&](map_store& map) {
            bool added;
            return map.put(keys[1], "changed", false, added) && !added &&
                map.put(keys[1], "ignored", true, added) && !added;
        }), "existing value changed ancestor counts");
        expected[keys[1]] = "changed";
        verify(database, expected, true);
    }
}

static void test_locality_and_large() {
    auto database = memory();
    fields expected;
    for (unsigned i = 0; i < 10000; ++i)
        expected[std::to_string(100000 + i)] = "v";
    seed(database, expected);
    database->reads = database->writes = 0;
    require(operation(database, "hash", [](map_store& map) {
        bool added;
        return map.put("105000", "changed", false, added) && !added;
    }), "local update");
    require(database->reads < 70 && database->writes == 1,
        "point update rewrote/scanned the Hash");
    expected["105000"] = "changed";
    const std::string key(40000, 'k'), value(1024 * 1024, 'v');
    require(operation(database, "hash", [&](map_store& map) {
        bool added;
        return map.put(key, value, false, added) && added;
    }), "large field/value");
    expected[key] = value;
    database->blobs = 0;
    require(operation(database, "hash", [&](map_store& map) {
        uint64_t length; bool found;
        return map.length("105000", length, found) && found && length == 7;
    }) && database->blobs == 0, "unrelated large values loaded");
    require(operation(database, "hash", [&](map_store& map) {
        uint64_t length; bool found;
        return map.length(key, length, found) && found &&
            length == value.size();
    }), "large field length");
    verify(database, expected, true);
}

static void test_allocation_failure() {
    fields expected;
    for (unsigned i = 0; i < 20; ++i)
        expected[std::to_string(i)] = "base";
    expected["large"] = std::string(40000, 'x');
    for (bool erase : {false, true}) {
        bool success = false;
        unsigned failures = 0;
        for (long budget = 0; budget < 800; ++budget) {
            auto database = memory();
            seed(database, expected);
            std::string before, after;
            require(database->get("hash", before), "failure root fixture");
            const std::string replacement(50000, 'r');
            allocation_budget = budget;
            db::batch_status result;
            try {
            result = database->transact_pages({"hash"},
                [&](page_transaction& tx) {
                map_store map(tx, "hash");
                bool changed;
                return map.open() && (erase ? map.erase("1", changed) :
                    map.put("large", replacement, false, changed)) &&
                    map.finish();
            });
            } catch (const std::bad_alloc&) {
                result = db::batch_status::error;
            }
            allocation_budget = -1;
            if (result == db::batch_status::ok) {
                auto committed = expected;
                if (erase) committed.erase("1");
                else committed["large"] = replacement;
                verify(database, committed, true);
                success = true; break;
            }
            ++failures;
            require(database->get("hash", after) && after == before,
                "allocation failure changed root");
            verify(database, expected, true);
        }
        // Allocation reuse can reduce the number of failing budgets. Every
        // failure above still has to preserve the complete committed value.
        require(success && failures > 0, "allocation sweep missed success");
    }
}

static void test_delete_all_and_set_kind() {
    for (bool reverse : {false, true}) {
        auto database = memory();
        fields expected;
        for (unsigned i = 0; i < 31; ++i)
            expected[std::to_string(i)] = std::string(200, 'v');
        seed(database, expected);
        std::vector<std::string> keys;
        for (const auto& item : expected) keys.push_back(item.first);
        if (reverse) std::reverse(keys.begin(), keys.end());
        for (const auto& key : keys) {
            require(operation(database, "hash", [&](map_store& map) {
                bool removed;
                return map.erase(key, removed) && removed;
            }), "ordered deletion");
            expected.erase(key);
            verify(database, expected, true);
        }
        require(database->transact_pages({"hash"}, [](page_transaction& tx) {
            std::string bytes;
            for (uint64_t id = 1; id < 100; ++id)
                if (tx.lookup("hash", id, bytes) != db::lookup_status::missing)
                    return false;
            return true;
        }) == db::batch_status::ok, "last field retained pages");
    }
    auto database = memory();
    require(operation(database, "set", [](map_store& map) {
        bool added, found;
        std::string out;
        return map.put("member", "", false, added) && added &&
            map.get("member", out, found) && found && out.empty();
    }, map_kind::set), "Set-kind empty payload");
    require(database->transact_pages({"set"}, [](page_transaction& tx) {
        map_store map(tx, "set", map_kind::hash);
        require(!map.open() && map.error() == map_store::status::wrong_kind,
            "wrong map kind accepted");
        return true;
    }) == db::batch_status::aborted, "ignored wrong kind committed");
}

static void overwrite64(std::string& bytes, size_t offset, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i)
        bytes[offset + i] = static_cast<char>(value >> ((7 - i) * 8));
}

static void test_corruption_and_exhaustion() {
    for (unsigned damage = 0; damage < 7; ++damage) {
        auto database = memory();
        seed(database, {{"base", "v"}, {"large", std::string(40000, 'x')}});
        require(database->transact_pages({"hash"}, [&](page_transaction& tx) {
            auto& entry = *tx.roots().find("hash");
            map_root root;
            require(map_page_codec::decode_root(entry.value, root) ==
                map_page_status::ok, "corruption root");
            if (damage < 2) {
                if (!damage) ++root.elements;
                else root.last_id = UINT64_MAX;
                entry.action = db::update_action::write;
                return map_page_codec::encode_root(root, entry.value) ==
                    map_page_status::ok;
            }
            if (damage == 4) return tx.erase("hash", root.tree);
            std::string bytes;
            map_node node;
            auto id = root.tree;
            require(tx.lookup("hash", id, bytes) == db::lookup_status::found &&
                map_page_codec::decode_node(bytes, node) ==
                    map_page_status::ok, "corruption node");
            if (damage >= 5) {
                if (!node.value.head) {
                    id = node.left ? node.left : node.right;
                    require(id && tx.lookup("hash", id, bytes) ==
                        db::lookup_status::found && map_page_codec::decode_node(
                            bytes, node) == map_page_status::ok, "large node");
                }
                require(tx.lookup("hash", node.value.head, bytes) ==
                    db::lookup_status::found, "corruption blob");
                if (damage == 5) overwrite64(bytes, 24, 1);
                else bytes[3] = 3; // Unknown PMB version.
                return tx.put("hash", node.value.head, bytes);
            }
            require(map_page_codec::encode_node(node, bytes) ==
                map_page_status::ok, "wide corruption fixture");
            if (damage == 2) overwrite64(bytes, 32, 99ULL << 32);
            else overwrite64(bytes, 16, node.id);
            return tx.put("hash", id, bytes);
        }) == db::batch_status::ok, "corruption injection");
        std::string before, after;
        require(database->get("hash", before), "corrupt root snapshot");
        require(database->transact_pages({"hash"}, [&](page_transaction& tx) {
            map_store map(tx, "hash");
            bool added;
            require(map.open() && !map.put(damage >= 5 ? "large" : "new",
                "changed", false, added), "corrupt tree mutation accepted");
            const auto expected = damage == 1 ? map_store::status::too_large :
                damage == 6 ? map_store::status::unsupported :
                    map_store::status::invalid;
            require(map.error() == expected, "corrupt error classification");
            return true; // Even ignored errors must poison the native commit.
        }) == db::batch_status::aborted, "corrupt transaction committed");
        require(database->get("hash", after) && after == before,
            "corrupt root silently rewritten");
    }
}

static void test_root_counts_on_writes() {
    for (unsigned mode = 0; mode < 4; ++mode) {
        auto database = memory();
        seed(database, {{"a", "x"}, {"b", "y"}});
        require(database->transact_pages({"hash"}, [](page_transaction& tx) {
            auto& entry = *tx.roots().find("hash");
            map_root root;
            if (map_page_codec::decode_root(entry.value, root) !=
                map_page_status::ok) return false;
            ++root.elements;
            entry.action = db::update_action::write;
            return map_page_codec::encode_root(root, entry.value) ==
                map_page_status::ok;
        }) == db::batch_status::ok, "root count fixture");
        std::string before, after;
        require(database->get("hash", before), "bad count snapshot");
        require(database->transact_pages({"hash"}, [&](page_transaction& tx) {
            map_store map(tx, "hash");
            require(map.open(), "root count open");
            bool changed;
            std::string value;
            const bool ok = mode < 2 ? map.put("a", "changed", mode == 1,
                changed) : mode == 2 ? map.erase("absent", changed) :
                    map.take("a", value, changed);
            require(!ok && map.error() == map_store::status::invalid,
                "write bypassed root count validation");
            return true; // Ignoring the error must still abort publication.
        }) == db::batch_status::aborted, "bad count write committed");
        require(database->get("hash", after) && before == after,
            "bad count was silently rewritten");
    }
}

static void test_child_reuse_scope(unsigned mode) {
    auto database = memory();
    const fields expected = {{"a", "x"}, {"b", "y"}, {"c", "z"}};
    seed(database, expected);
    require(database->transact_pages({"hash"}, [&](page_transaction& tx) {
        map_store map(tx, "hash");
        bool changed, found;
        std::string value;
        require(map.open() && map.put("a", "updated", false, changed) &&
            !changed && map.finish(), "reuse scope update");
        require(map.get("a", value, found) && found && value == "updated",
            "reused child hid staged value");
        map_node root, child;
        std::string bytes;
        require(tx.lookup("hash", map.metadata().tree, bytes) ==
            db::lookup_status::found && map_page_codec::decode_node(bytes,
                root) == map_page_status::ok, "reuse scope root");
        const auto id = root.left ? root.left : root.right;
        require(id && tx.lookup("hash", id, bytes) == db::lookup_status::found
            && map_page_codec::decode_node(bytes, child) == map_page_status::ok,
            "reuse scope child");
        ++child.elements;
        require(map_page_codec::encode_node(child, bytes) ==
            map_page_status::ok && tx.put("hash", id, std::move(bytes)),
            "reuse scope corrupt child");
        const bool ok = mode == 0 ? map.put("a", "ignored", true, changed) :
            mode == 1 ? map.erase("absent", changed) :
                map.take("a", value, changed);
        require(!ok &&
            map.error() == map_store::status::invalid,
            "cross-operation child reuse hid corruption");
        return true;
    }) == db::batch_status::aborted, "corrupt child update committed");
    verify(database, expected, true);
}

static void test_shared_descendant_update() {
    auto database = memory();
    std::vector<std::string> keys;
    for (unsigned i = 0; keys.size() < 8; ++i) {
        const auto key = "alias:" + std::to_string(i);
        if (scan_bucket(key) == 0) keys.push_back(key);
    }
    std::sort(keys.begin(), keys.end());
    const uint64_t left[] = {2, 4, 5, 6, 0, 0};
    const uint64_t right[] = {3, 3, 0, 0, 0, 0};
    const uint32_t height[] = {4, 3, 2, 2, 1, 1};
    const uint64_t counts[] = {8, 5, 2, 2, 1, 1};
    const unsigned rank[] = {7, 2, 4, 1, 3, 0};
    std::vector<std::string> before(7);
    map_root root;
    root.tree = 1; root.last_id = 6; root.elements = 8;
    require(map_page_codec::encode_root(root, before[0]) ==
        map_page_status::ok, "alias root encode");
    require(database->transact_pages({"hash"}, [&](page_transaction& tx) {
        auto& entry = *tx.roots().find("hash");
        entry.value = before[0]; entry.action = db::update_action::write;
        for (unsigned i = 0; i < 6; ++i) {
            map_node node;
            node.id = i + 1; node.left = left[i]; node.right = right[i];
            node.height = height[i]; node.elements = counts[i];
            node.key.inline_value = keys[rank[i]];
            node.key.length = node.key.inline_value.size();
            node.value.inline_value = "v"; node.value.length = 1;
            require(map_page_codec::encode_node(node, before[i + 1]) ==
                map_page_status::ok, "alias node encode");
            require(tx.put("hash", node.id, before[i + 1]), "alias stage");
        }
        return true;
    }) == db::batch_status::ok, "alias fixture");
    for (bool erase : {false, true}) {
        require(database->transact_pages({"hash"}, [&](page_transaction& tx) {
            map_store map(tx, "hash", map_kind::hash, false);
            bool added;
            require(map.open(), "shared descendant open");
            const bool ok = erase ? map.erase(keys[3], added) :
                map.put(keys[5], "new", false, added);
            require(!ok &&
                map.error() == map_store::status::invalid,
                "shared descendant update was accepted");
            // Ignoring the failure must not publish partial writes.
            return true;
        }) == db::batch_status::aborted, "shared descendant commit");
    }
    require(database->transact_pages({"hash"}, [&](page_transaction& tx) {
        require(tx.roots().find("hash")->value == before[0], "alias root");
        for (unsigned i = 1; i < before.size(); ++i) {
            std::string out;
            require(tx.lookup("hash", i, out) == db::lookup_status::found &&
                out == before[i], "alias failure changed stored page");
        }
        std::string out;
        return tx.lookup("hash", 7, out) == db::lookup_status::missing;
    }) == db::batch_status::ok, "alias failure leaked new node");
}

static void test_finish_and_concurrency() {
    auto database = memory();
    seed(database, {{"base", "v"}});
    require(database->transact_pages({"hash"}, [](page_transaction& tx) {
        map_store map(tx, "hash");
        bool added;
        return map.open() && map.put("forgotten", "v", false, added);
    }) == db::batch_status::aborted, "unfinished context committed");
    verify(database, {{"base", "v"}}, true);
    std::vector<std::thread> workers;
    for (unsigned t = 0; t < 4; ++t) workers.emplace_back([&, t] {
        for (unsigned f = 0; f < 4; ++f) go[&, t, f] {
            for (unsigned i = 0; i < 20; ++i) {
                const auto key = std::to_string(t) + ":" + std::to_string(f) +
                    ":" + std::to_string(i);
                require(operation(database, "hash", [&](map_store& map) {
                    bool added, found;
                    std::string out;
                    if (!map.put(key, key, false, added) || !added)
                        return false;
                    acl::fiber::yield();
                    return map.get(key, out, found) && found && out == key &&
                        map.erase(key, found) && found;
                }), "concurrent read-own-pages");
            }
        };
        acl::fiber::schedule();
    });
    for (auto& worker : workers) worker.join();
    verify(database, {{"base", "v"}}, true);
}

class metadata_error_db : public tracking_db {
public:
    uint64_t rejected_id = 0;
protected:
    bool dbborrows_pages() const override { return true; }
    lookup_status dbinspect_page(const std::string& owner, uint64_t id,
        std::string& scratch, page_inspection& inspection) override {
        const auto result = mdb::dbinspect_page(owner, id, scratch, inspection);
        return id == rejected_id ? lookup_status::error : result;
    }
};

static void test_metadata_error_priority() {
    for (bool backend_error : {false, true}) {
        auto engine = std::make_shared<metadata_error_db>();
        shared_db database = engine;
        require(database->open(""), "metadata priority open");
        seed(database, {{"a", "x"}, {"b", "y"}, {"c", "z"}});
        uint64_t child_id = 0;
        std::string corrupt, original, probe;
        require(database->get("hash", original), "metadata root snapshot");
        require(database->transact_page("hash", [&](page_transaction& tx) {
            map_root root; map_node node;
            std::string bytes;
            if (map_page_codec::decode_root(original, root) !=
                map_page_status::ok || tx.lookup("hash", root.tree, bytes) !=
                db::lookup_status::found || map_page_codec::decode_node(bytes,
                    node) != map_page_status::ok) return false;
            child_id = node.left ? node.left : node.right;
            probe = node.key.inline_value;
            if (node.left && node.right) {
                map_node sibling;
                if (tx.lookup("hash", node.right, bytes) !=
                    db::lookup_status::found || map_page_codec::decode_node(
                        bytes, sibling) != map_page_status::ok) return false;
                probe = sibling.key.inline_value;
            }
            if (!child_id || tx.lookup("hash", child_id, bytes) !=
                db::lookup_status::found || map_page_codec::decode_node(bytes,
                    node) != map_page_status::ok) return false;
            node.bucket ^= 1;
            return map_page_codec::encode_node(node, corrupt) ==
                map_page_status::ok && tx.put("hash", child_id, corrupt);
        }) == db::batch_status::ok, "metadata corrupt child fixture");
        engine->rejected_id = backend_error ? child_id : 0;
        require(database->transact_page("hash", [&](page_transaction& tx) {
            map_store store(tx, "hash", map_kind::hash, false);
            std::string value = "unchanged";
            bool found;
            require(store.open() && !store.get(probe, value, found) &&
                !found && value == "unchanged" &&
                store.error() == (backend_error ? map_store::status::storage :
                    map_store::status::invalid),
                "lookup accepted a corrupt sibling");
            return true;
        }) == (backend_error ? db::batch_status::error :
            db::batch_status::aborted), "failed lookup committed");
        require(database->transact_pages({"hash", "peer"},
            [&](page_transaction& tx) {
            auto& peer = *tx.roots().find("peer");
            peer.value = "unpublished";
            peer.action = db::update_action::write;
            if (!tx.put("peer", 1, "unpublished")) return false;
            map_store store(tx, "hash", map_kind::hash, false);
            std::string key = "unchanged";
            bool found;
            require(store.open() && store.set_expiry(expiry + 1) &&
                !store.select(0, key, found) && !found && key == "unchanged"
                && !store.finish() && store.error() == (backend_error ?
                    map_store::status::storage : map_store::status::invalid),
                "metadata validation masked backend error");
            return true; // Ignoring either error must not publish peer data.
        }) == (backend_error ? db::batch_status::error :
            db::batch_status::aborted), "metadata failure committed");
        engine->rejected_id = 0;
        std::string after;
        require(database->get("hash", after) && after == original &&
            !database->get("peer", after), "metadata failure changed roots");
    }
}

static void test_node_view() {
    map_node node;
    node.id = 7; node.left = 2; node.right = 3;
    node.height = 2; node.elements = 3;
    for (size_t keys = 0; keys <= 64; ++keys)
    for (size_t values = 0; values <= 128; ++values) {
        node.key.inline_value.assign(keys, char(keys)); node.key.length = keys;
        node.value.inline_value.assign(values, char(values));
        node.value.length = values;
        std::string bytes;
        require(map_page_codec::encode_node(node, bytes) ==
            map_page_status::ok, "node view fixture");
        map_node_view view;
        allocation_budget = 0;
        const auto result = map_page_codec::decode_node_view(bytes, view);
        allocation_budget = -1;
        require(result == map_page_status::ok && view.id == node.id &&
            view.left == node.left && view.right == node.right &&
            view.height == node.height && view.elements == node.elements &&
            view.key.data == bytes.data() + 80 && view.value.data ==
                bytes.data() + 80 + keys &&
            std::string(view.key.data, view.key.inline_size()) ==
                node.key.inline_value &&
            std::string(view.value.data, view.value.inline_size()) ==
                node.value.inline_value, "node view copied or changed data");
    }
    std::string bytes;
    require(map_page_codec::encode_node(node, bytes) == map_page_status::ok,
        "node view truncation fixture");
    map_node_view sentinel;
    sentinel.id = 99; sentinel.key.data = "keep";
    for (size_t size = 0; size < bytes.size(); ++size) {
        auto view = sentinel;
        require(map_page_codec::decode_node_view(bytes.substr(0, size),
            view) != map_page_status::ok && view.id == sentinel.id &&
            view.key.data == sentinel.key.data,
            "truncated view changed output");
    }
    for (size_t offset = 0; offset < 80; ++offset) {
        auto corrupt = bytes;
        corrupt[offset] ^= char(255);
        auto view = sentinel;
        map_node owning;
        const auto expected = map_page_codec::decode_node(corrupt, owning);
        require(map_page_codec::decode_node_view(corrupt, view) == expected,
            "view validation differs from owning decode");
        if (expected != map_page_status::ok)
            require(view.id == sentinel.id && view.key.data ==
                sentinel.key.data, "invalid view changed output");
    }
    node.key.head = 10; node.key.length = 1000;
    node.key.inline_value.clear();
    node.value.head = 11; node.value.length = 2000;
    node.value.inline_value.clear();
    require(map_page_codec::encode_node(node, bytes) == map_page_status::ok,
        "external view fixture");
    map_node_view view;
    require(map_page_codec::decode_node_view(bytes, view) ==
        map_page_status::ok && view.key.head == 10 && view.value.head == 11 &&
        !view.key.inline_size() && !view.value.inline_size(),
        "external view tried reading inline payload");
}

static void test_node_encode_reuse() {
    map_node node;
    node.id = 1; node.left = 2; node.right = 3;
    node.height = 2; node.elements = 3; node.bucket = 7;
    node.key.inline_value.assign(64, 'k'); node.key.length = 64;
    node.key.inline_value[9] = '\0';
    node.value.inline_value.assign(128, 'v'); node.value.length = 128;
    std::string expected, output = "keep";
    require(map_page_codec::encode_node(node, expected) ==
        map_page_status::ok, "node encode reuse fixture");
    output.reserve(expected.size());
    allocation_budget = 0;
    const auto result = map_page_codec::encode_node(node, output);
    allocation_budget = -1;
    require(result == map_page_status::ok && output == expected,
        "warm node encode allocated or changed bytes");
    std::string cold = "keep";
    allocation_budget = 0;
    const auto failed = map_page_codec::encode_node(node, cold);
    allocation_budget = -1;
    require(failed == map_page_status::out_of_memory && cold == "keep",
        "failed node encode changed output");
    auto key_alias = node, value_alias = node;
    require(map_page_codec::encode_node(key_alias,
        key_alias.key.inline_value) == map_page_status::ok &&
        key_alias.key.inline_value == expected, "node key output alias");
    require(map_page_codec::encode_node(value_alias,
        value_alias.value.inline_value) == map_page_status::ok &&
        value_alias.value.inline_value == expected, "node value output alias");
    node.left = node.id;
    require(map_page_codec::encode_node(node, output) ==
        map_page_status::invalid && output == expected,
        "invalid node changed reused output");
}

static void test_packed_staging_encode() {
    for (bool hashed : {false, true}) {
        map_node node;
        node.id = 1;
        node.key.inline_value.assign(hashed ? 19 : 16, 'k');
        node.key.inline_value[5] = '\0';
        node.key.length = node.key.inline_value.size();
        if (hashed) {
            node.bucket = static_cast<uint8_t>(scan_bucket(node.key.inline_value));
            node.value.inline_value.assign(8, 'v'); node.value.length = 8;
        }
        std::string expected, output;
        require(map_page_codec::encode_packed_node(node, expected, hashed) ==
            map_page_status::ok && expected[3] == (hashed ? 12 : 11),
            "compact staging fixture");
        output.reserve(64);
        const auto capacity = output.capacity();
        allocation_budget = 0;
        const auto ok = map_page_codec::encode_packed_node_scratch(node, output, hashed);
        allocation_budget = -1;
        require(ok == map_page_status::ok && output == expected &&
            output.capacity() == capacity, "compact staging discarded warm capacity");
        auto key_alias = node, value_alias = node;
        require(map_page_codec::encode_packed_node_scratch(key_alias,
            key_alias.key.inline_value, hashed) == map_page_status::ok &&
            key_alias.key.inline_value == expected, "staging key alias");
        require(map_page_codec::encode_packed_node_scratch(value_alias,
            value_alias.value.inline_value, hashed) == map_page_status::ok &&
            value_alias.value.inline_value == expected, "staging value alias");
        std::string cold = "keep";
        allocation_budget = 0;
        const auto failed = map_page_codec::encode_packed_node_scratch(node, cold, hashed);
        allocation_budget = -1;
        require(failed == map_page_status::out_of_memory && cold == "keep",
            "staging allocation failure changed output");
        output.assign(4096, 'x');
        require(map_page_codec::encode_packed_node_scratch(node, output, hashed) ==
            map_page_status::ok && output == expected && output.capacity() <= 128,
            "compact staging retained oversized capacity");
        node.left = node.id;
        require(map_page_codec::encode_packed_node_scratch(node, output, hashed) ==
            map_page_status::invalid && output == expected,
            "invalid staging node changed output");
    }
}

static void test_write_buffers() {
    using workspace = detail::node_write_workspace;
    {
        std::array<workspace, 96> held;
        for (size_t i = 0; i < held.size(); ++i)
            held[i].buffer().assign(128, static_cast<char>(i));
        {
            workspace nested;
            nested.buffer().assign(256, 'n');
            for (size_t i = 0; i < held.size(); ++i)
                require(held[i].buffer() == std::string(128, char(i)),
                    "nested writer reused live buffer");
        }
    }
    require(workspace::retained_entries() == workspace::max_entries &&
        workspace::retained_capacity() <= workspace::max_entries *
            workspace::max_capacity, "write buffer cache is unbounded");
    {
        workspace oversized;
        require(oversized.buffer().empty(), "cached write contents leaked");
        oversized.buffer().assign(4096, 'x');
    }
    require(workspace::retained_capacity() <= workspace::max_entries *
        workspace::max_capacity, "oversized write buffer retained");
    std::thread compact([] {
        constexpr size_t page_size = 40;
        {
            workspace writer;
            writer.buffer().assign(page_size, 'p');
        }
        require(workspace::retained_entries() == 1,
            "compact write buffer was not recycled");
        workspace writer;
        require(writer.buffer().empty() &&
            writer.buffer().capacity() >= page_size,
            "compact write buffer was not reused empty");
        allocation_budget = 0;
        writer.buffer().assign(page_size, 'q');
        allocation_budget = -1;
    });
    compact.join();
    std::thread late([] {
        struct final_writer {
            std::string bytes;
            ~final_writer() {
                workspace writer;
                writer.buffer().assign(128, 'l');
                workspace::recycle(bytes);
                require(workspace::retained_entries() == 0,
                    "write buffer cache reopened after shutdown");
            }
        };
        // Register this destructor before initializing the buffer cache.
        static thread_local final_writer owner;
        owner.bytes.assign(128, 'o');
        workspace warm;
        warm.buffer().assign(128, 'w');
    });
    late.join();
}

static void test_root_buffer_reuse() {
    map_root root;
    root.elements = 1; root.tree = 1; root.last_id = uint64_t(UINT16_MAX) + 1;
    std::string expected, out = "keep";
    require(map_page_codec::encode_root(root, expected) ==
        map_page_status::ok, "root reuse fixture");
    out.reserve(48);
    allocation_budget = 0;
    const auto result = map_page_codec::encode_root(root, out);
    allocation_budget = -1;
    require(result == map_page_status::ok && out == expected,
        "Map root failed to reuse output capacity");
    root.expires_at_ms = -2;
    require(map_page_codec::encode_root(root, out) == map_page_status::invalid
        && out == expected, "invalid Map root changed reused output");
    root.expires_at_ms = -1;
    std::string cold = "keep";
    allocation_budget = 0;
    const auto failed = map_page_codec::encode_root(root, cold);
    allocation_budget = -1;
    require(failed == map_page_status::out_of_memory && cold == "keep",
        "Map root allocation failure changed output");
}

class read_workspace_db : public tracking_db {
public:
    std::function<void()> during_read;
    unsigned failure = 0;
protected:
    lookup_status dblookup_page(const std::string& owner, uint64_t id,
        std::string& out) override {
        require(out.empty(), "node backend received old contents");
        const auto result = tracking_db::dblookup_page(owner, id, out);
        if (during_read) {
            auto callback = std::move(during_read);
            callback();
        }
        if (failure) {
            out.assign(failure == 1 ? 256 : 65536, 'x');
            return failure == 1 ? lookup_status::error : lookup_status::found;
        }
        return result;
    }
};

static void read_field(shared_db database, const std::string& expected) {
    require(operation(database, "hash", [&](map_store& map) {
        bool found;
        std::string out;
        return map.get("a", out, found) && found && out == expected;
    }), "node workspace field differs");
}

static void test_read_workspace() {
    using workspace = detail::node_read_workspace;
    {
        workspace outer;
        auto& bytes = outer.buffer(); bytes.assign(256, 'a');
        {
            workspace inner;
            inner.buffer().assign(512, 'b');
            require(bytes == std::string(256, 'a'), "nested node bytes alias");
        }
        require(bytes == std::string(256, 'a'), "nested node release changed");
    }
    {
        workspace next;
        require(next.buffer().empty(), "node workspace retained contents");
        next.buffer().assign(65536, 'x');
    }
    require(workspace::retained_capacity() <= map_page_codec::max_page_bytes,
        "node workspace retained oversized capacity");
    auto database = std::make_shared<read_workspace_db>();
    require(database->open(""), "node workspace memory open");
    seed(database, {{"a", "outer"}});
    read_field(database, "outer");
    require(operation(database, "hash", [](map_store& map) {
        bool found;
        std::string out;
        allocation_budget = 0;
        const bool ok = map.get("a", out, found);
        allocation_budget = -1;
        require(ok && found && out == "outer", "warm node read allocated");
        return true;
    }), "warm node read failed");
    for (unsigned failure : {1U, 2U}) {
        database->failure = failure;
        require(database->transact_page("hash", [](page_transaction& tx) {
            map_store map(tx, "hash");
            bool found;
            std::string out;
            require(map.open() && !map.get("a", out, found) &&
                map.error() == map_store::status::storage,
                "node scratch error accepted");
            return true;
        }) == db::batch_status::error, "node scratch error not poisoned");
        database->failure = 0;
        require(workspace::retained_capacity() <=
            map_page_codec::max_page_bytes, "bad node capacity retained");
        read_field(database, "outer");
    }
    require(operation(database, "hash", [](map_store& map) {
        bool added;
        return map.put("a", "changed", false, added) && !added;
    }), "node workspace replacement");
    read_field(database, "changed");
    std::cout << "warm node read: 0 allocations\n";
}

static void test_read_workspace_yield() {
    auto outer = std::make_shared<read_workspace_db>();
    auto nested = memory(), peer = memory();
    require(outer->open(""), "yield node memory open");
    seed(outer, {{"a", "outer"}});
    seed(nested, {{"a", "nested"}});
    seed(peer, {{"a", "peer"}});
    bool entered = false, finished = false;
    outer->during_read = [&] {
        read_field(nested, "nested");
        entered = true;
        while (!finished) acl::fiber::yield();
    };
    go[&] { read_field(outer, "outer"); };
    go[&] {
        while (!entered) acl::fiber::yield();
        read_field(peer, "peer");
        finished = true;
    };
    acl::fiber::schedule();
    require(entered && finished, "node read did not yield to peer");
    read_field(outer, "outer");
    read_field(nested, "nested");
    read_field(peer, "peer");
}

static void require_node_bytes(const map_node& node,
    const std::string& expected, const char* reason) {
    std::string bytes;
    require(map_page_codec::encode_node(node, bytes) == map_page_status::ok &&
        bytes == expected, reason);
}

static void test_node_decode_scratch() {
    map_node source;
    source.id = 1; source.left = 2; source.right = 3;
    source.height = 2; source.elements = 3; source.bucket = 7;
    source.key.inline_value.assign(64, 'k'); source.key.length = 64;
    source.value.inline_value.assign(128, 'v'); source.value.length = 128;
    std::string bytes;
    require(map_page_codec::encode_node(source, bytes) == map_page_status::ok,
        "scratch node fixture");
    map_node scratch;
    scratch.key.inline_value.reserve(64);
    scratch.value.inline_value.reserve(128);
    allocation_budget = 0;
    auto result = map_page_codec::decode_node_scratch(bytes, scratch);
    allocation_budget = -1;
    require(result == map_page_status::ok, "scratch node read allocated");
    require_node_bytes(scratch, bytes, "scratch node contents");
    map_node strong;
    strong.id = 9;
    std::string unchanged;
    require(map_page_codec::encode_node(strong, unchanged) ==
        map_page_status::ok, "strong node fixture");
    allocation_budget = 0;
    result = map_page_codec::decode_node(bytes, strong);
    allocation_budget = -1;
    require(result == map_page_status::out_of_memory, "cold node allocation");
    require_node_bytes(strong, unchanged, "strong node failure changed out");
    std::string invalid = bytes; invalid[15] = 0;
    require(map_page_codec::decode_node(invalid, strong) ==
        map_page_status::invalid, "invalid strong node accepted");
    require_node_bytes(strong, unchanged, "invalid strong node changed out");
    require(map_page_codec::decode_node_scratch(invalid, scratch) ==
        map_page_status::invalid, "invalid scratch node accepted");
    allocation_budget = 0;
    result = map_page_codec::decode_node_scratch(bytes, scratch);
    allocation_budget = -1;
    require(result == map_page_status::ok, "scratch failed to recover");
    require_node_bytes(scratch, bytes, "scratch recovery changed contents");
    map_node cold;
    allocation_budget = 1;
    result = map_page_codec::decode_node_scratch(bytes, cold);
    allocation_budget = -1;
    require(result == map_page_status::out_of_memory,
        "scratch partial allocation did not fail");
    require(map_page_codec::decode_node_scratch(bytes, cold) ==
        map_page_status::ok, "scratch partial allocation recovery");
    require_node_bytes(cold, bytes, "scratch partial allocation contents");
    for (bool value : {false, true}) {
        map_node alias; alias.id = 99;
        auto& input = value ? alias.value.inline_value : alias.key.inline_value;
        input = bytes;
        allocation_budget = 0;
        result = map_page_codec::decode_node_scratch(input, alias);
        allocation_budget = -1;
        require(result == map_page_status::out_of_memory &&
            alias.id == 99 && input == bytes, "aliased scratch error changed");
        require(map_page_codec::decode_node_scratch(input, alias) ==
            map_page_status::ok, "aliased scratch decode failed");
        require_node_bytes(alias, bytes, "aliased scratch decode contents");
    }
    std::string owned = bytes;
    require(map_page_codec::decode_node_scratch(owned, scratch) ==
        map_page_status::ok, "scratch ownership decode");
    owned.assign(owned.size(), 'x');
    require_node_bytes(scratch, bytes, "scratch borrowed encoded input");
    source.key.inline_value.clear(); source.key.length = 129;
    source.key.head = 4; source.value.inline_value.clear();
    source.value.length = 129; source.value.head = 5;
    require(map_page_codec::encode_node(source, owned) == map_page_status::ok,
        "scratch fragmented fixture");
    allocation_budget = 0;
    result = map_page_codec::decode_node_scratch(owned, scratch);
    allocation_budget = -1;
    require(result == map_page_status::ok && scratch.key.inline_value.empty()
        && scratch.value.inline_value.empty(), "scratch retained old inline");
    require_node_bytes(scratch, owned, "scratch fragmented contents");
    std::cout << "scratch node decode: 0 allocations; strong errors intact\n";
}

static void test_decoded_node_capacity() {
    auto database = memory();
    const std::string expected(64, 'v');
    fields values;
    for (unsigned i = 0; i < 1000; ++i)
        values.emplace(std::to_string(100000 + i), expected);
    seed(database, values);
    auto read = [&](const std::string& key, bool measure) {
        long allocations = 0;
        require(operation(database, "hash", [&](map_store& map) {
            bool found;
            std::string out;
            if (measure) allocation_budget = LONG_MAX;
            const bool ok = map.get(key, out, found);
            if (measure) allocations = LONG_MAX - allocation_budget;
            allocation_budget = -1;
            return ok && found && out == expected;
        }), "decoded node capacity read");
        return allocations;
    };
    read("100000", false);
    long maximum = 0;
    for (unsigned i = 0; i < 100; ++i)
        maximum = std::max(maximum, read(std::to_string(100000 + i * 7), true));
    require(maximum > 0 && maximum <= 4,
        "decoded payload allocations grew with traversal depth");
    std::cout << "wide64 Map get maximum allocations: " << maximum << '\n';
}

static void test_live_bound_roots() {
    auto database = memory();
    const std::string prefix(80, 'b');
    const std::string left = prefix + std::string("\0a", 2);
    const std::string right = prefix + std::string("\0b", 2);
    require(database->transact_pages({right, left}, [&](page_transaction& tx) {
        for (const auto& key : {left, right}) {
            map_store map(tx, key);
            bool added;
            if (!map.open()) return false;
            map.initialize();
            if (!map.put("field", key == left ? "left" : "right", false,
                added) || !added || !map.finish()) return false;
        }
        return true;
    }) == db::batch_status::ok, "bound roots seed");
    auto read = [](map_store& map, const std::string& expected) {
        std::string value; bool found;
        return map.get("field", value, found) && found && value == expected;
    };
    require(database->transact_pages({right, left}, [&](page_transaction& tx) {
        map_store a(tx, left), b(tx, right);
        if (!a.open() || !b.open() || !read(a, "left") || !read(b, "right"))
            return false;
        std::string bytes;
        const auto id = a.metadata().tree;
        if (tx.lookup(left, id, bytes) != db::lookup_status::found)
            return false;
        map_node node;
        if (map_page_codec::decode_node(bytes, node) != map_page_status::ok)
            return false;
        node.value.inline_value = "new"; node.value.length = 3;
        if (map_page_codec::encode_node(node, bytes) != map_page_status::ok ||
            !tx.reset_pages(left) || !tx.put(left, id, std::move(bytes)))
            return false;
        tx.roots().find(left)->action = db::update_action::write;
        const auto reads = database->reads;
        if (!read(a, "new") || database->reads != reads || !read(b, "right"))
            return false;
        bool added;
        return b.put("extra", "v", false, added) && added &&
            read(a, "new") && read(b, "right") && a.finish() && b.finish();
    }) == db::batch_status::ok, "bound roots staged reset and owner routing");
    for (bool erase : {false, true}) {
        require(database->transact_pages({left}, [&](page_transaction& tx) {
            map_store map(tx, left);
            if (!map.open() || !read(map, "new")) return false;
            if (erase) tx.roots().find(left)->action = db::update_action::erase;
            else require(tx.reset_pages(left), "bound root clear fixture");
            std::string value = "keep"; bool found;
            require(!map.get("field", value, found) && !found &&
                value == "keep" && map.error() == map_store::status::invalid,
                "bound read ignored live root flags");
            return true;
        }) == db::batch_status::aborted, "bound root flags error committed");
    }
    require(database->transact_pages({left}, [&](page_transaction& tx) {
        map_store map(tx, left);
        if (!map.open() || !read(map, "new")) return false;
        const auto reads = database->reads;
        tx.abort();
        bool added;
        require(!map.put("field", "ignored", false, added) &&
            database->reads == reads, "bound read ignored transaction abort");
        return true;
    }) == db::batch_status::aborted, "aborted bound root write committed");
    require(operation(database, left, [&](map_store& map) {
        return read(map, "new");
    }) && operation(database, right, [&](map_store& map) {
        std::string value; bool found;
        return read(map, "right") && map.get("extra", value, found) &&
            found && value == "v";
    }), "bound roots rollback changed data");
    std::cout <<
        "bound roots: live flags, staged pages, binary owners passed\n";
}

static void test_key_buffer_workspace() {
    using workspace = detail::node_key_workspace;
    {
        std::array<std::unique_ptr<workspace>, workspace::max_entries> live;
        for (auto& node : live) {
            node.reset(new workspace(true));
            node->key.inline_value.assign(64, 'k');
        }
    }
    require(workspace::retained_entries() == workspace::max_entries &&
        workspace::retained_capacity() <= workspace::max_entries *
            workspace::max_capacity, "key capacity pool exceeded bounds");
    map_node source;
    source.id = 1; source.key.length = 64;
    source.key.inline_value.assign(64, 'x');
    std::string encoded;
    require(map_page_codec::encode_node(source, encoded) ==
        map_page_status::ok, "key capacity fixture encode");
    bool ok = false;
    allocation_budget = 0;
    {
        workspace node(true);
        ok = node.key.inline_value.empty() &&
            map_page_codec::decode_node_scratch(encoded, node) ==
                map_page_status::ok &&
            node.key.inline_value == source.key.inline_value;
    }
    allocation_budget = -1;
    require(ok, "warm key decode allocated or reused old payload");
    const auto count = workspace::retained_entries();
    {
        workspace oversized(true, false);
        oversized.key.inline_value.assign(1024, 'z');
    }
    require(workspace::retained_entries() == count,
        "oversized key capacity was retained");
    {
        workspace disabled(false);
        disabled.key.inline_value.assign(64, 'd');
    }
    require(workspace::retained_entries() == count,
        "disabled key workspace touched cache");
    {
        workspace outer(true);
        outer.key.inline_value.assign(48, 'a');
        {
            workspace inner(true);
            require(inner.key.inline_value.empty(), "nested key not empty");
            inner.key.inline_value.assign(48, 'b');
            require(outer.key.inline_value == std::string(48, 'a'),
                "nested key buffer aliases active owner");
        }
        require(outer.key.inline_value == std::string(48, 'a'),
            "nested destruction changed live key");
    }
    bool results[4]{};
    std::thread workers[4];
    for (unsigned i = 0; i < 4; ++i) workers[i] = std::thread([&, i] {
        {
            workspace seed(true, false);
            seed.key.inline_value.assign(40, char('a' + i));
        }
        bool valid = workspace::retained_entries() == 1;
        {
            workspace node(true);
            valid = valid && node.key.inline_value.empty() &&
                workspace::retained_entries() == 0;
            node.key.inline_value.assign(40, char('a' + i));
        }
        results[i] = valid && workspace::retained_entries() == 1;
    });
    for (auto& worker : workers) worker.join();
    for (bool valid : results) require(valid, "key pool crossed threads");
    bool stopped = false;
    std::thread late([&] {
        struct probe {
            bool* stopped;
            ~probe() {
                bool valid;
                {
                    workspace node(true);
                    valid = node.key.inline_value.empty() &&
                        workspace::retained_capacity() == 0;
                    node.key.inline_value.assign(40, 'q');
                }
                *stopped = valid && workspace::retained_capacity() == 0;
            }
        };
        static thread_local probe guard{&stopped};
        (void)guard;
        workspace node(true, false);
        node.key.inline_value.assign(40, 'z');
    });
    late.join();
    require(stopped, "key capacity cache reopened after TLS cleanup");
}

static void test_scan_bucket_compatibility() {
    std::mt19937 random(1729);
    std::array<char, 1024> bytes;
    for (auto& byte : bytes) byte = static_cast<char>(random() & 255);
    for (size_t start = 0; start < 8; ++start) {
        uint64_t hash = 14695981039346656037ULL;
        for (size_t length = 0; length < bytes.size() - start; ++length) {
            require(scan_bucket(bytes.data() + start, length) == (hash & 255),
                "scan bucket changed from the stored FNV byte");
            hash ^= static_cast<unsigned char>(bytes[start + length]);
            hash *= 1099511628211ULL;
        }
    }
}

int main() {
    acl::acl_cpp_init();
    test_metadata_error_priority();
    test_scan_bucket_compatibility();
    test_node_view();
    test_node_encode_reuse();
    test_packed_staging_encode();
    test_write_buffers();
    test_root_buffer_reuse();
    test_codec();
    test_hashed_key_leaf();
    test_model();
    test_value_fragment_reuse();
    test_insert_rotations();
    test_locality_and_large();
    test_allocation_failure();
    test_finish_and_concurrency();
    test_delete_all_and_set_kind();
    test_corruption_and_exhaustion();
    test_root_counts_on_writes();
    for (unsigned mode = 0; mode < 3; ++mode)
        test_child_reuse_scope(mode);
    test_shared_descendant_update();
    test_read_workspace();
    test_read_workspace_yield();
    test_node_decode_scratch();
    test_decoded_node_capacity();
    test_live_bound_roots();
    test_key_buffer_workspace();
    std::cout << "bounded map pages and AVL operations passed\n";
}
