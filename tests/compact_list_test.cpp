#include "stdafx.h"
#include "coder/list_page.h"
#include "coder/compact_list_root.h"
#include "db/list_store.h"
#include "db/mdb/mdb.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <new>
#include <random>

static thread_local long budget = -1;
void* operator new(std::size_t n) {
    if (!budget) throw std::bad_alloc();
    if (budget > 0) --budget;
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
#if __cplusplus >= 201402L
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
#endif
using namespace pkv;
using codec = list_page_codec;
using status = list_page_status;
static void require(bool ok, const char* text) {
    if (!ok) { std::cerr << text << '\n'; std::exit(1); }
}
static list_fragment member(const std::string& value) {
    list_fragment result; result.total_bytes = value.size();
    result.bytes = value; return result;
}
static bool equal(const list_page& a, const list_page& b) {
    if (a.id != b.id || a.previous != b.previous || a.next != b.next ||
        a.fragments.size() != b.fragments.size()) return false;
    for (size_t i = 0; i < a.fragments.size(); ++i) {
        const auto& x = a.fragments[i]; const auto& y = b.fragments[i];
        if (x.total_bytes != y.total_bytes || x.offset != y.offset ||
            x.bytes != y.bytes) return false;
    }
    return true;
}
static void test_roundtrip_and_edits() {
    std::mt19937 random(175493);
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        list_page page; page.id = 7;
        const unsigned count = 2 + random() % 100;
        for (unsigned i = 0; i < count; ++i) {
            std::string value(random() % 80, '\0');
            for (auto& byte : value) byte = static_cast<char>(random());
            page.fragments.push_back(member(value));
        }
        std::string bytes, old;
        require(codec::encode_packed_page(page, bytes) == status::ok &&
            bytes[3] == 3 && codec::encode_page(page, old) == status::ok &&
            old[3] == 1 && old.size() - bytes.size() == 23 * count,
            "compact exact savings and legacy writer");
        list_page decoded;
        require(codec::decode_page(bytes, decoded) == status::ok &&
            equal(page, decoded), "compact roundtrip");
        const std::vector<std::string> values{"", std::string("a\0b", 3)};
        for (bool left : {false, true}) {
            auto expected = page;
            for (const auto& value : values) {
                if (left) expected.fragments.insert(expected.fragments.begin(),
                    member(value));
                else expected.fragments.push_back(member(value));
            }
            std::string encoded;
            list_page_update update;
            require(codec::encode_packed_page(expected, encoded) ==
                status::ok && codec::push_end(bytes, values, left, 1024,
                    update) == status::ok && update.applied &&
                update.bytes == encoded, "compact push order/format");
            expected = page;
            const auto value = left ? expected.fragments.front().bytes :
                expected.fragments.back().bytes;
            if (left) expected.fragments.erase(expected.fragments.begin());
            else expected.fragments.pop_back();
            require(codec::encode_packed_page(expected, encoded) ==
                status::ok && codec::pop_end(bytes, left, 1024, update) ==
                status::ok && update.applied && update.bytes == encoded &&
                update.value == value, "compact pop order/format");
            if (count > 2) {
                std::vector<std::string> wanted{value};
                wanted.push_back(left ? expected.fragments.front().bytes :
                    expected.fragments.back().bytes);
                if (left) expected.fragments.erase(expected.fragments.begin());
                else expected.fragments.pop_back();
                std::vector<std::string> popped;
                require(codec::encode_packed_page(expected, encoded) ==
                    status::ok && codec::pop_compact_batch(bytes, left, 2,
                        1024, update, &popped) == status::ok && update.applied &&
                    update.bytes == encoded && popped == wanted,
                    "compact batch pop order/format");
            }
        }
    }
}
static void test_uniform_lengths() {
    for (size_t size : {size_t(0), size_t(7)}) {
        list_page page;
        page.id = 7;
        page.fragments.assign(17, member(std::string(size, 'x')));
        page.fragments.back() = member(std::string(size, 'y'));
        std::string bytes;
        list_page_update update;
        require(codec::encode_packed_page(page, bytes) == status::ok &&
            codec::pop_end(bytes, false, size, update) == status::ok &&
            update.applied && update.value == page.fragments.back().bytes,
            "uniform compact right pop");
        auto corrupt = bytes;
        corrupt[codec::page_header_bytes + 16 * (1 + size)] =
            static_cast<char>(size + 1);
        require(codec::pop_end(corrupt, false, 1024, update) ==
            status::invalid, "uniform compact tail corruption");
        std::vector<std::string> popped;
        require(codec::pop_compact_batch(bytes, false, 2, size, update,
            &popped) == status::ok && update.applied &&
            popped == std::vector<std::string>{std::string(size, 'y'),
                std::string(size, 'x')}, "uniform batch right pop order");
        list_page remaining;
        require(codec::decode_page(update.bytes, remaining) == status::ok &&
            remaining.fragments.size() == 15 &&
            remaining.fragments.back().bytes == std::string(size, 'x'),
            "uniform batch right pop tail");
    }
    list_page page;
    page.id = 7;
    page.fragments.assign(17, member("abcd"));
    page.fragments[15] = member("abc");
    page.fragments[16] = member("abcde");
    std::string bytes;
    list_page_update update;
    require(codec::encode_packed_page(page, bytes) == status::ok &&
        codec::pop_end(bytes, false, 5, update) == status::ok &&
        update.value == "abcde", "uniform compact mixed-length fallback");
    require(codec::pop_end(bytes, false, 4, update) == status::invalid,
        "uniform compact mixed-length member bound");
}

static void test_corruption_and_oom() {
    list_page page; page.id = 7;
    page.fragments = {member(std::string(100, 'a')), member("")};
    std::string bytes;
    require(codec::encode_packed_page(page, bytes) == status::ok, "fixture");
    auto reject = [&](const std::string& bad) {
        list_page decoded = page;
        require(codec::decode_page(bad, decoded) != status::ok &&
            equal(page, decoded), "bad compact decode changed output");
        for (bool left : {false, true}) {
            list_page_update out;
            out.id = 999; out.bytes = "keep"; out.value = "value";
            require(codec::push_end(bad, {"x"}, left, 1000, out) !=
                status::ok && out.id == 999 && out.bytes == "keep" &&
                out.value == "value", "bad compact push changed output");
            require(codec::pop_end(bad, left, 1000, out) != status::ok &&
                out.id == 999 && out.bytes == "keep" && out.value == "value",
                "bad compact pop changed output");
        }
    };
    for (size_t n = 0; n < bytes.size(); ++n) reject(bytes.substr(0, n));
    reject(bytes + 'x');
    for (size_t at : {size_t(4), size_t(32)}) {
        auto bad = bytes; bad[at] = '\xff'; reject(bad);
    }
    auto self = bytes; self[23] = 7; reject(self);
    std::mt19937 random(148293);
    for (unsigned i = 0; i < 10000; ++i) {
        auto bad = bytes;
        for (unsigned j = 0; j < 4; ++j)
            bad[random() % bad.size()] = static_cast<char>(random());
        list_page decoded = page;
        if (codec::decode_page(bad, decoded) == status::ok) {
            std::string canonical;
            require(codec::encode_packed_page(decoded, canonical) ==
                status::ok && canonical == bad, "noncanonical compact data");
        } else require(equal(page, decoded), "fuzz changed compact output");
    }
    const std::vector<std::string> values{std::string(100, 'b')};
    for (int operation = 0; operation < 4; ++operation) {
        bool passed = false;
        for (long available = 0; available < 16; ++available) {
            list_page decoded; decoded.id = 999;
            list_page_update update; update.id = 999; update.bytes = "keep";
            std::string out = "keep";
            budget = available;
            status result;
            if (!operation) result = codec::decode_page(bytes, decoded);
            else if (operation == 1)
                result = codec::encode_packed_page(page, out);
            else if (operation == 2)
                result = codec::push_end(bytes, values, true, 1000, update);
            else result = codec::pop_end(bytes, true, 1000, update);
            budget = -1;
            if (result == status::ok) { passed = true; break; }
            require(result == status::out_of_memory && decoded.id == 999 &&
                decoded.fragments.empty() && out == "keep" &&
                update.id == 999 && update.bytes == "keep",
                "compact OOM changed output");
        }
        require(passed, "compact allocation sweep never succeeded");
    }
    list_page_update update; update.id = 999;
    require(codec::pop_end(bytes, true, 99, update) == status::invalid &&
        codec::push_end(bytes, values, false, 99, update) == status::invalid &&
        update.id == 999, "compact member bound bypassed");
}
static void test_expanded_bound_and_fallback() {
    list_page page; page.id = 7;
    page.fragments.assign(681, member(""));
    std::string bytes;
    require(codec::encode_packed_page(page, bytes) == status::ok &&
        bytes.size() == codec::page_header_bytes + 681,
        "compact maximum empty count");
    list_page_update update;
    require(codec::push_end(bytes, {""}, false, 10, update) == status::ok &&
        !update.applied, "compact expanded bound ignored");
    page.fragments.push_back(member(""));
    require(codec::encode_packed_page(page, bytes) == status::invalid,
        "oversized expanded compact page accepted");
    page.fragments.assign(74, member("abc"));
    require(codec::encode_packed_page(page, bytes) == status::ok &&
        codec::push_end(bytes, {"new"}, false, 10, update, 2048) == status::ok
        && !update.applied, "compact logical fill target ignored");
    page.fragments = {member(std::string(codec::max_fragment_bytes, 'x'))};
    require(codec::encode_packed_page(page, bytes) == status::ok &&
        codec::pop_end(bytes, true, codec::max_fragment_bytes, update) ==
            status::ok && !update.applied, "single member fallback");
    page.fragments[0].bytes.push_back('x');
    ++page.fragments[0].total_bytes;
    require(codec::encode_packed_page(page, bytes) == status::invalid,
        "expanded payload bound ignored");
    page.fragments = {member("part")}; page.fragments[0].total_bytes = 10;
    page.next = 8;
    std::string old;
    require(codec::encode_packed_page(page, bytes) == status::ok &&
        bytes[3] == 1 && codec::encode_page(page, old) == status::ok &&
        old == bytes, "fragmented page no longer uses legacy format");
}
static void test_mixed_native_pages() {
    auto database = std::make_shared<mdb>(8);
    require(database->open(""), "mixed database open");
    require(database->transact_page("mixed", [&](page_transaction& tx) {
        list_root root; root.elements = 3; root.head = 1; root.tail = 2;
        root.last_id = 2;
        auto* entry = tx.roots().find("mixed");
        if (!entry || codec::encode_root(root, entry->value) != status::ok)
            return false;
        entry->action = db::update_action::write;
        list_page first; first.id = 1; first.next = 2;
        first.fragments = {member("a"), member("b")};
        list_page second; second.id = 2; second.previous = 1;
        second.fragments = {member("c")};
        std::string legacy, compact;
        return codec::encode_page(first, legacy) == status::ok &&
            codec::encode_packed_page(second, compact) == status::ok &&
            tx.put("mixed", 1, legacy) && tx.put("mixed", 2, compact);
    }) == db::batch_status::ok, "mixed seed");
    std::vector<std::string> values;
    require(database->transact_page("mixed", [&](page_transaction& tx) {
        list_store list(tx, "mixed");
        return list.open() && list.push({"front"}, true) &&
            list.push({"back"}, false) && list.pop(3, true, &values) &&
            values == std::vector<std::string>({"front", "a", "b"}) &&
            list.range(0, -1, values) &&
            values == std::vector<std::string>({"c", "back"}) && list.finish();
    }) == db::batch_status::ok, "mixed legacy/compact staged operations");
}
static void test_cached_pop(bool left) {
    list_page page;
    page.id = 43;
    for (unsigned i = 0; i < 112; ++i)
        page.fragments.push_back(member(std::string(i % 57, char(i))));
    std::string bytes;
    require(codec::encode_packed_page(page, bytes) == status::ok,
        "cached pop fixture");
    while (page.fragments.size() > 1) {
        if (!left && page.fragments.size() == 77) {
            std::vector<std::string> expected, actual;
            for (size_t i = 0; i < 7; ++i) {
                expected.push_back(page.fragments.back().bytes);
                page.fragments.pop_back();
            }
            list_page_update batch;
            std::string encoded;
            require(codec::pop_compact_batch(bytes, false, 7, 100, batch,
                &actual) == status::ok && batch.applied && actual == expected &&
                codec::encode_packed_page(page, encoded) == status::ok &&
                batch.bytes == encoded, "cached batch disagreed with scalar pops");
            bytes.swap(batch.bytes);
        }
        const auto expected = left ? page.fragments.front().bytes :
            page.fragments.back().bytes;
        if (left) page.fragments.erase(page.fragments.begin());
        else page.fragments.pop_back();
        std::string encoded;
        list_page_update out;
        require(codec::pop_end(bytes, left, 100, out) == status::ok &&
            out.applied && out.value == expected &&
            codec::encode_packed_page(page, encoded) == status::ok &&
            out.bytes == encoded, "cached pop changed values or layout");
        bytes.swap(out.bytes);
        if (page.fragments.size() == (left ? 20 : 80)) {
            list_page_update rejected;
            rejected.bytes = "keep";
            require(codec::pop_end(bytes, left, 8, rejected) ==
                status::invalid && rejected.bytes == "keep",
                "cached pop ignored a tighter member limit");
            const auto first_length = bytes[codec::page_header_bytes];
            bytes[codec::page_header_bytes] = '\xff';
            require(codec::pop_end(bytes, left, 100, rejected) ==
                status::invalid && rejected.bytes == "keep",
                "cached pop accepted different malformed bytes");
            bytes[codec::page_header_bytes] = first_length;
        }
    }
}

static void test_pop_removed_neighbor() {
    for (bool left : {false, true}) {
        list_page page;
        page.id = 7;
        (left ? page.previous : page.next) = 9;
        page.fragments = {member("first"), member("middle"), member("last")};
        std::string bytes;
        require(codec::encode_packed_page(page, bytes) == status::ok,
            "linked pop fixture");
        list_page_update update;
        std::vector<std::string> popped;
        std::string expected;
        for (size_t width : {64, 1024}) {
            auto linked = page;
            linked.fragments.assign(3, member(std::string(width, 'x')));
            std::string input;
            require(codec::encode_packed_page(linked, input) == status::ok,
                "unlink fixture");
            (left ? linked.previous : linked.next) = 0;
            require(codec::encode_packed_page(linked, expected) == status::ok,
                "unlink reference");
            update.bytes = "keep";
            require(codec::pop_compact_batch(input, left, 0, 1024, update,
                &popped, 8) == status::invalid && update.bytes == "keep",
                "unlink accepted the wrong backlink");
            budget = 0;
            const auto result = codec::pop_compact_batch(input, left, 0, 1024,
                update, &popped, 9);
            budget = -1;
            require(result == status::out_of_memory && update.bytes == "keep" &&
                popped.empty(), "failed unlink changed its output");
            require(codec::pop_compact_batch(input, left, 0, 1024, update,
                &popped, 9) == status::ok && update.applied && !update.consumed &&
                update.bytes == expected && popped.empty(),
                "unlink changed the surviving members");
        }
        require(codec::pop_compact_batch(bytes, left, 1, 1024, update,
            &popped, 8) == status::invalid && popped.empty(),
            "linked pop accepted the wrong backlink");
        const auto value = left ? "first" : "last";
        if (left) page.fragments.erase(page.fragments.begin());
        else page.fragments.pop_back();
        (left ? page.previous : page.next) = 0;
        require(codec::encode_packed_page(page, expected) == status::ok &&
            codec::pop_compact_batch(bytes, left, 1, 1024, update,
                &popped, 9) == status::ok && update.applied &&
            update.bytes == expected && popped == std::vector<std::string>{value},
            "linked pop changed member order or endpoint bytes");
        std::vector<std::string> whole{"first", "middle", "last"};
        if (!left) std::reverse(whole.begin(), whole.end());
        whole.insert(whole.begin(), value);
        require(codec::pop_compact_batch(bytes, left, 4, 1024, update,
            &popped, 9) == status::ok && !update.applied &&
            update.consumed == 3 && update.bytes.empty() && popped == whole,
            "whole linked page did not return all members in pop order");

        (left ? page.previous : page.next) = 9;
        page.fragments.assign(3, member(std::string(64, 'x')));
        require(codec::encode_packed_page(page, bytes) == status::ok,
            "linked pop allocation fixture");
        popped.assign(1, "keep");
        popped.reserve(4);
        budget = 1;
        const auto result = codec::pop_compact_batch(bytes, left, 3, 1024,
            update, &popped, 9);
        budget = -1;
        require(result == status::out_of_memory &&
            popped == std::vector<std::string>{"keep"},
            "linked pop allocation failure changed the result prefix");

        (left ? page.previous : page.next) = 0;
        require(codec::encode_packed_page(page, bytes) == status::ok &&
            codec::pop_compact_batch(bytes, left, 3, 1024, update, &popped) ==
                status::ok && !update.applied && !update.consumed &&
            popped == std::vector<std::string>{"keep"},
            "endpoint pop changed default whole-page fallback");
        require(codec::pop_compact_batch(bytes, left, 3, 1024, update,
            &popped, 0, true) == status::ok && !update.applied &&
            update.consumed == 3 && update.bytes.empty() &&
            popped == std::vector<std::string>(3, std::string(64, 'x')),
            "endpoint pop did not extract the complete page");
    }
}

static void test_full_endpoint_link() {
    for (bool left : {false, true}) {
        list_page page;
        page.id = 7;
        page.fragments.assign(72, member("xxxx"));
        const std::string short_value = "next";
        std::string bytes, expected;
        list_page_update update;
        require(codec::encode_packed_page(page, bytes) == status::ok,
            "full endpoint fixture");
        (left ? page.previous : page.next) = 8;
        require(codec::encode_packed_page(page, expected) == status::ok &&
            codec::push_end(bytes, list_values_view(&short_value, 1), left,
                1024, update, 2048, 8) == status::ok && update.linked &&
            !update.applied && update.bytes == expected,
            "full endpoint did not preserve encoded members");
        page.previous = page.next = 0;
        page.fragments.pop_back();
        const std::string wide(700, 'x');
        require(codec::encode_packed_page(page, bytes) == status::ok &&
            codec::push_end(bytes, list_values_view(&wide, 1), left,
                1024, update, 2048, 8) == status::ok && !update.linked &&
            !update.applied, "fragmentable member skipped remaining space");
    }
}

static void test_single_member_encoding() {
    list_page page;
    page.id = 7; page.previous = 3;
    for (const auto& value : {std::string(), std::string("a\0b", 3),
        std::string(1024, 'x')}) {
        auto fixture = page;
        fixture.fragments.push_back(member(value));
        std::string expected;
        auto aliased = value;
        require(codec::encode_packed_page(fixture, expected) == status::ok &&
            codec::encode_single_member(page, aliased, aliased) == status::ok &&
            aliased == expected, "single member encoded differently");
    }
}

static void test_root_offset_growth() {
    using namespace compact_list_root;
    const std::string a(127, 'a'), b(127, 'b'), longer(180, 'e');
    const std::vector<std::string> added{std::string(100, 'c'), std::string(100, 'd')};
    const list_values_view input(added.data(), added.size());
    require(encode_push(input, false) == encode({added[0], added[1]}) &&
        encode_push(input, true) == encode({added[1], added[0]}),
        "direct push encoding preserves list order");
    const auto narrow = encode({a, b}, 987);
    require(!(byte(narrow[4]) & wide_offsets_flag), "256-byte payload stays narrow");
    for (bool left : {false, true}) {
        auto root = narrow;
        require(view(root).extend(list_values_view(added.data(), added.size()), left, root),
            "compact root widens during push");
        const auto expected = left ? encode({added[1], added[0], a, b}, 987)
                                   : encode({a, b, added[0], added[1]}, 987);
        require(root == expected && (byte(root[4]) & wide_offsets_flag),
            "wide root offsets and order after push");
        const auto original = view(root).slice(left ? 2 : 0, 2);
        require(original == narrow, "slice restores narrow root");
        const auto remaining = view(root).without(left ? 2 : 4, 3);
        require(view(remaining).count() == 3 && view(remaining).deadline() == 987 &&
            view(remaining).at(1).str() == (left ? a : b), "wide root removal");
        view(root).replace_deadline(root, -1);
        require(view(root).deadline() == -1 && view(root).at(left ? 3 : 1).str() == b,
            "wide root TTL removal preserves offsets");
    }
    std::string replaced;
    require(view(narrow).replace(0, longer, replaced) &&
        replaced == encode({longer, b}, 987), "compact root widens during LSET");

    const std::string full(32, 'f'), tail(34, 't'), empty, abc = "abc", xyz = "xyz";
    std::vector<member_view> members(61, full);
    auto large = encode(members, 987);
    const list_values_view last(&tail, 1);
    members.push_back(tail);
    require(view(large).extend(last, false, large) &&
        large == encode(members, 987) &&
        view(large).at(61).str() == tail, "2048-byte payload stays compact");
    const auto snapshot = large;
    members.push_back(empty);
    require(!view(large).extend(list_values_view(&empty, 1), false, large) &&
        large == snapshot && encode(members).empty(),
        "payloads above 2048 bytes require paging");
    const std::string wide_value(255, 'w');
    require(!encode({wide_value, wide_value, wide_value, wide_value}).empty() &&
        encode({wide_value, wide_value, wide_value, wide_value, wide_value}).empty(),
        "low-density payloads keep the 1024-byte limit");
    members.assign(max_members, abc);
    members.back() = xyz;
    auto many = encode(members, 987);
    require(view(many).count() == max_members &&
        !view(many).extend(list_values_view(&empty, 1), true, many),
        "compact List member limit");
    members.pop_back();
    require(view(many).without(uint64_t{1} << (max_members - 1), max_members - 1)
        == encode(members, 987), "compact List removal above bit 31");
}

int main() {
    acl::acl_cpp_init();
    test_roundtrip_and_edits(); test_corruption_and_oom();
    test_uniform_lengths();
    test_expanded_bound_and_fallback(); test_mixed_native_pages();
    test_cached_pop(true); test_cached_pop(false);
    test_full_endpoint_link();
    test_pop_removed_neighbor();
    test_single_member_encoding();
    test_root_offset_growth();
    std::cout << "Compact List: legacy, bounds, fuzz, OOM, mixed passed\n";
}
