#include "coder/list_page.h"
#include "coder/validated_page_cache.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <random>
#include <thread>

// Sweep every allocation boundary, including fragment payload allocation.
static thread_local long allocation_budget = -1;
void* operator new(std::size_t size) {
    if (allocation_budget == 0) {
        throw std::bad_alloc();
    }
    if (allocation_budget > 0) {
        --allocation_budget;
    }
    if (void* pointer = std::malloc(size ? size : 1)) {
        return pointer;
    }
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
#if __cplusplus >= 201402L
void operator delete(void* pointer, std::size_t) noexcept {
    std::free(pointer);
}
void operator delete[](void* pointer, std::size_t) noexcept {
    std::free(pointer);
}
#endif

using namespace pkv;
using status = list_page_status;
using codec = list_page_codec;

static void require(bool ok, const char* reason) {
    if (!ok) {
        std::cerr << reason << '\n';
        std::exit(1);
    }
}

static list_fragment fragment(const std::string& bytes, uint64_t offset = 0,
    uint64_t total = 0) {
    list_fragment out;
    out.total_bytes = total ? total : bytes.size();
    out.offset = offset;
    out.bytes = bytes;
    return out;
}

static bool equal(const list_page& first, const list_page& second) {
    if (first.id != second.id || first.previous != second.previous ||
        first.next != second.next ||
        first.fragments.size() != second.fragments.size()) {
        return false;
    }
    for (size_t i = 0; i < first.fragments.size(); ++i) {
        const auto& a = first.fragments[i];
        const auto& b = second.fragments[i];
        if (a.total_bytes != b.total_bytes || a.offset != b.offset ||
            a.bytes != b.bytes) {
            return false;
        }
    }
    return true;
}

static void test_root() {
    list_root root, decoded;
    std::string encoded;
    require(codec::encode_root(root, encoded) == status::ok,
        "empty root encode");
    require(encoded.size() == 40 && encoded.substr(0, 8) ==
        std::string("PLR\1\0\0\0\0", 8), "root exact header");
    require(encoded.substr(8, 8) == std::string(8, '\xff'),
        "persistent expiry encoding");
    require(codec::decode_root(encoded, decoded) == status::ok &&
        decoded.expires_at_ms == -1 && decoded.elements == 0,
        "empty root decode");
    root.expires_at_ms = 4102444800123;
    root.elements = 1000000;
    root.head = 0x0102030405060708ULL;
    root.tail = 2;
    require(codec::encode_root(root, encoded) == status::ok &&
        encoded.substr(24, 8) ==
            std::string("\1\2\3\4\5\6\7\10", 8),
        "root network byte order");
    require(codec::decode_root(encoded, decoded) == status::ok &&
        decoded.elements == root.elements && decoded.head == root.head &&
        decoded.tail == 2 && decoded.expires_at_ms == root.expires_at_ms,
        "populated root roundtrip");
    const auto valid = encoded;
    for (size_t length = 0; length < valid.size(); ++length) {
        decoded = root;
        require(codec::decode_root(valid.substr(0, length), decoded) !=
            status::ok && decoded.head == root.head,
            "truncated root changed output");
    }
    for (size_t offset : {size_t(4), size_t(8), size_t(16)}) {
        encoded = valid;
        encoded[offset] = '\x80';
        require(codec::decode_root(encoded, decoded) == status::invalid,
            "reserved/expiry/count root validation");
    }
    encoded = valid;
    encoded[3] = 3;
    require(codec::decode_root(encoded, decoded) ==
        status::unsupported_version, "future root version");
    root.head = 0;
    encoded = "unchanged";
    require(codec::encode_root(root, encoded) == status::invalid &&
        encoded == "unchanged", "invalid root changed output");
    root.elements = 0;
    root.tail = 0;
    root.expires_at_ms = -2;
    require(codec::encode_root(root, encoded) == status::invalid,
        "negative expiry accepted");
}

static void test_extended_root() {
    list_root root;
    root.elements = 5;
    root.head = 1;
    root.tail = 2;
    root.last_id = 99;
    std::string bytes;
    require(codec::encode_root(root, bytes) == status::ok &&
        bytes.size() == 48 && bytes[3] == 2 &&
        static_cast<unsigned char>(bytes.back()) == 99,
        "extended root/counter encode");
    const auto original = bytes;
    list_root decoded;
    require(codec::decode_root(bytes, decoded) == status::ok &&
        decoded.last_id == 99 && decoded.elements == 5,
        "extended root/counter decode");
    for (size_t size = 0; size < original.size(); ++size) {
        decoded = root;
        require(codec::decode_root(original.substr(0, size), decoded) !=
            status::ok && decoded.last_id == 99,
            "truncated extended root changed output");
    }
    bool preserves_deadline = false;
    ++root.elements;
    require(codec::encode_root(root, bytes, &preserves_deadline) == status::ok &&
        preserves_deadline, "native count change lost persistent deadline");
    root.expires_at_ms = 4102444800123;
    require(codec::encode_root(root, bytes, &preserves_deadline) == status::ok &&
        !preserves_deadline, "new deadline reported as preserved");
    --root.elements;
    require(codec::encode_root(root, bytes, &preserves_deadline) == status::ok &&
        preserves_deadline, "native count change lost existing deadline");
    bytes = original;
    bytes[47] = 0;
    require(codec::decode_root(bytes, decoded) == status::invalid,
        "zero-counter extended root accepted");
    bytes[47] = 1;
    require(codec::decode_root(bytes, decoded) == status::invalid,
        "counter below endpoint accepted");
    root.last_id = 1;
    bytes = "sentinel";
    require(codec::encode_root(root, bytes) == status::invalid &&
        bytes == "sentinel", "invalid counter changed output");
}

static void test_page() {
    list_page page, decoded;
    page.id = 1;
    page.fragments = {fragment(""), fragment(std::string("x\0y", 3)),
        fragment("other")};
    std::string encoded;
    require(codec::encode_page(page, encoded) == status::ok &&
        encoded.substr(0, 8) == std::string("PLP\1\0\0\0\3", 8),
        "page count/network header");
    require(codec::decode_page(encoded, decoded) == status::ok &&
        equal(page, decoded), "empty/binary member roundtrip");
    page.fragments = {fragment(std::string(codec::max_fragment_bytes, 'x'))};
    require(codec::encode_page(page, encoded) == status::ok &&
        encoded.size() == codec::max_page_bytes,
        "exact page size boundary rejected");
    page.fragments.front().bytes.push_back('x');
    ++page.fragments.front().total_bytes;
    require(codec::encode_page(page, encoded) == status::invalid,
        "oversized page accepted");
    page.fragments = {fragment("ab", 2, 6), fragment("cd", 4, 6),
        fragment("next")};
    require(codec::encode_page(page, encoded) == status::invalid,
        "continuation without predecessor");
    page.previous = 2;
    require(codec::encode_page(page, encoded) == status::ok &&
        codec::decode_page(encoded, decoded) == status::ok &&
        equal(page, decoded), "continued fragment roundtrip");
    page.fragments[1].offset = 3;
    require(codec::encode_page(page, encoded) == status::invalid,
        "overlapping fragment accepted");
    page.fragments = {fragment("x", 0, UINT64_MAX)};
    page.next = 3;
    require(codec::encode_page(page, encoded) == status::ok &&
        codec::decode_page(encoded, decoded) == status::ok,
        "huge logical member allocated or rejected");
    page.fragments[0].offset = UINT64_MAX;
    require(codec::encode_page(page, encoded) == status::invalid,
        "fragment overflow accepted");
    page.fragments = {fragment("x", UINT64_MAX - 1, UINT64_MAX)};
    require(codec::encode_page(page, encoded) == status::ok,
        "maximum member end rejected");
    page.next = 0;
    require(codec::encode_page(page, encoded) == status::ok,
        "completed continuation requires successor");
    page.previous = page.id;
    require(codec::encode_page(page, encoded) == status::invalid,
        "self-link accepted");
    page.previous = 2;
    page.next = 2;
    require(codec::encode_page(page, encoded) == status::invalid,
        "two-node link cycle accepted");
}

static void test_corruption() {
    list_page page, decoded;
    page.id = 7;
    page.fragments = {fragment(std::string(100, 'a')), fragment("")};
    std::string encoded;
    require(codec::encode_page(page, encoded) == status::ok, "fixture");
    const auto original = encoded;
    for (size_t length = 0; length < original.size(); ++length) {
        decoded = page;
        require(codec::decode_page(original.substr(0, length), decoded) !=
            status::ok && equal(page, decoded),
            "truncated page changed output");
    }
    for (size_t offset : {size_t(4), size_t(32), size_t(40), size_t(48),
        size_t(52)}) {
        encoded = original;
        encoded[offset] = '\xff';
        decoded = page;
        require(codec::decode_page(encoded, decoded) == status::invalid &&
            equal(page, decoded), "malformed page changed output");
    }
    encoded = original + 'x';
    require(codec::decode_page(encoded, decoded) == status::invalid,
        "trailing bytes accepted");
    encoded = original;
    encoded[3] = 4;
    require(codec::decode_page(encoded, decoded) ==
        status::unsupported_version, "future page version");
    std::mt19937 random(75421);
    for (size_t attempt = 0; attempt < 10000; ++attempt) {
        encoded = original;
        for (unsigned i = 0; i < 4; ++i) {
            encoded[random() % encoded.size()] =
                static_cast<char>(random());
        }
        decoded = page;
        if (codec::decode_page(encoded, decoded) == status::ok) {
            std::string reencoded;
            require(codec::encode_page(decoded, reencoded) == status::ok &&
                reencoded == encoded, "accepted noncanonical page");
        } else {
            require(equal(page, decoded), "corruption changed output");
        }
    }
}

static void test_fragmented_member() {
    // A 1 MiB binary member is represented by many bounded physical values.
    // Build/read one page at a time, never a whole-list serialized buffer.
    const std::string member(1024 * 1024, '\x80');
    const size_t count = (member.size() + codec::max_fragment_bytes - 1) /
        codec::max_fragment_bytes;
    size_t recovered = 0;
    for (size_t i = 0; i < count; ++i) {
        list_page page, decoded;
        page.id = i + 1;
        page.previous = i == 0 ? 0 : i;
        page.next = i + 1 == count ? 0 : i + 2;
        const auto size = std::min(codec::max_fragment_bytes,
            member.size() - recovered);
        page.fragments = {fragment(member.substr(recovered, size),
            recovered, member.size())};
        std::string bytes;
        require(codec::encode_page(page, bytes) == status::ok &&
            bytes.size() <= codec::max_page_bytes,
            "large member exceeded bounded page");
        require(codec::decode_page(bytes, decoded) == status::ok &&
            decoded.fragments.front().offset == recovered &&
            decoded.fragments.front().bytes == member.substr(recovered, size),
            "fragmented member roundtrip");
        recovered += size;
    }
    require(recovered == member.size(), "fragmented member incomplete");
}

static void test_allocation_failure() {
    list_page page;
    page.id = 1;
    for (unsigned i = 0; i < 16; ++i) {
        page.fragments.push_back(fragment(std::string(100, 'x')));
    }
    std::string bytes;
    require(codec::encode_page(page, bytes) == status::ok, "fixture");
    bool succeeded = false;
    for (long budget = 0; budget < 128; ++budget) {
        list_page decoded;
        decoded.id = 999;
        allocation_budget = budget;
        const auto result = codec::decode_page(bytes, decoded);
        allocation_budget = -1;
        if (result == status::ok) {
            require(equal(page, decoded), "successful decode mismatch");
            succeeded = true;
            break;
        }
        require(result == status::out_of_memory && decoded.id == 999 &&
            decoded.fragments.empty(), "failed decode changed output");
    }
    require(succeeded, "allocation sweep never decoded");
    std::string encoded = "sentinel";
    allocation_budget = 0;
    const auto result = codec::encode_page(page, encoded);
    allocation_budget = -1;
    require(result == status::out_of_memory && encoded == "sentinel",
        "failed encode changed output");
}

static void test_encoded_push() {
    list_page page;
    page.id = 7;
    for (unsigned i = 0; i < 500; ++i)
        page.fragments.push_back(fragment(std::string(i % 7, char(i))));
    std::string bytes;
    require(codec::encode_page(page, bytes) == status::ok, "dense fixture");
    const std::vector<std::string> values = {"", std::string("a\0b", 3),
        "last"};
    for (bool left : {false, true}) {
        auto expected = page;
        for (const auto& value : values) {
            if (left)
                expected.fragments.insert(expected.fragments.begin(),
                    fragment(value));
            else expected.fragments.push_back(fragment(value));
        }
        std::string encoded;
        require(codec::encode_page(expected, encoded) == status::ok,
            "expected push encoding");
        list_page_update update;
        require(codec::push_end(bytes, values, left, 1024, update) ==
            status::ok && update.applied && update.id == page.id &&
            update.bytes == encoded, "direct push changed format/order");
    }
    list_page_update update;
    const std::vector<std::string> huge = {std::string(16384, 'x')};
    require(codec::push_end(bytes, huge, false, 20000, update) ==
        status::ok && !update.applied && update.id == 7 &&
        update.bytes.empty(), "full page did not request fallback");
    require(codec::push_end(bytes, {}, false, 20000, update) ==
        status::ok && !update.applied, "empty push applied");
}

static void rejected_push(const std::string& bytes,
    const std::vector<std::string>& values, bool left, uint64_t limit,
    status expected) {
    list_page_update update;
    update.id = 999; update.previous = 998; update.next = 1000;
    update.applied = true; update.bytes = "sentinel";
    const auto result = codec::push_end(bytes, values, left, limit, update);
    require(result == expected && update.id == 999 &&
        update.previous == 998 && update.next == 1000 && update.applied &&
        update.bytes == "sentinel", "rejected push changed output");
}

static void test_encoded_push_failures() {
    list_page page;
    page.id = 7;
    page.fragments = {fragment("abc")};
    std::string bytes;
    require(codec::encode_page(page, bytes) == status::ok, "push fixture");
    const std::vector<std::string> values = {"x"};
    for (size_t i = 0; i < bytes.size(); ++i)
        rejected_push(bytes.substr(0, i), values, false, 10,
            status::invalid);
    for (size_t offset : {size_t(4), size_t(32), size_t(40), size_t(48),
        size_t(52)}) {
        auto bad = bytes;
        bad[offset] = '\xff';
        rejected_push(bad, values, false, 10, status::invalid);
    }
    auto future = bytes;
    future[3] = 4;
    rejected_push(future, values, false, 10, status::unsupported_version);
    rejected_push(bytes, values, false, 2, status::invalid);
    rejected_push(bytes, {"oversize"}, false, 4, status::invalid);
    allocation_budget = 0;
    list_page_update update;
    update.id = 999;
    const auto result = codec::push_end(bytes, values, false, 10, update);
    allocation_budget = -1;
    require(result == status::out_of_memory && update.id == 999 &&
        !update.applied && update.bytes.empty(), "push OOM changed output");
    page.previous = 6;
    require(codec::encode_page(page, bytes) == status::ok, "linked fixture");
    rejected_push(bytes, values, true, 10, status::invalid);
    page.previous = 0; page.next = 8;
    require(codec::encode_page(page, bytes) == status::ok, "linked fixture");
    rejected_push(bytes, values, false, 10, status::invalid);
    page.fragments = {fragment("a", 0, 100)};
    require(codec::encode_page(page, bytes) == status::ok,
        "large logical member fixture");
    rejected_push(bytes, values, true, 10, status::invalid);
}

static void test_complete_after_unfinished_fragment() {
    list_page page;
    page.id = 7;
    page.fragments = {fragment("a"), fragment("")};
    std::string bytes;
    require(codec::encode_page(page, bytes) == status::ok,
        "complete fragment fixture");
    // Make the first fragment unfinished, followed by a complete empty member.
    // The second member must not bypass the required continuation check.
    bytes[codec::page_header_bytes + 7] = 2;
    list_page decoded;
    decoded.id = 999;
    require(codec::decode_page(bytes, decoded) == status::invalid &&
        decoded.id == 999, "empty member bypassed continuation check");
    for (bool left : {false, true}) {
        rejected_push(bytes, {"x"}, left, 10, status::invalid);
        list_page_update update;
        update.id = 999; update.bytes = "sentinel";
        require(codec::pop_end(bytes, left, 10, update) == status::invalid &&
            update.id == 999 && update.bytes == "sentinel",
            "pop bypassed continuation check");
    }
}

static void test_encoded_pop() {
    std::mt19937 random(38149);
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        list_page page;
        page.id = 7;
        const unsigned count = 2 + random() % 500;
        for (unsigned i = 0; i < count; ++i)
            page.fragments.push_back(fragment(std::string(random() % 8,
                static_cast<char>(random()))));
        std::string bytes;
        require(codec::encode_page(page, bytes) == status::ok,
            "pop dense fixture");
        for (bool left : {false, true}) {
            auto expected = page;
            const auto value = left ? expected.fragments.front().bytes :
                expected.fragments.back().bytes;
            if (left) expected.fragments.erase(expected.fragments.begin());
            else expected.fragments.pop_back();
            std::string encoded;
            require(codec::encode_page(expected, encoded) == status::ok,
                "expected pop encoding");
            list_page_update update;
            require(codec::pop_end(bytes, left, 1024, update) == status::ok &&
                update.applied && update.bytes == encoded &&
                update.value == value, "direct pop changed format/value");
        }
    }
}

static void test_native_fragment_pop() {
    list_page page;
    page.id = 7;
    page.previous = 9;
    page.fragments = {fragment("suffix", 4090, 4096),
        fragment(std::string(4097, 'x'))};
    std::string bytes;
    require(codec::encode_page(page, bytes) == status::ok, "fragment pop fixture");
    const auto original = bytes;
    list_page_update reference, update;
    require(codec::pop_end(bytes, false, 4097, reference) == status::ok &&
        reference.applied, "fragment pop reference");
    pkv::list_page_edit plan;
    update.value.reserve(4097);
    allocation_budget = 0;
    const auto prepared = codec::pop_end(bytes, false, 4097, update, &plan);
    allocation_budget = -1;
    require(prepared == status::ok && plan.id == page.id &&
        plan.length_bytes == codec::fragment_header_bytes &&
        update.applied && update.bytes.empty() && update.value == reference.value,
        "fragment pop plan did not reuse the output buffer");
    auto wrong = plan;
    ++wrong.right_stride;
    require(codec::apply_edit(bytes, wrong) == status::invalid && bytes == original,
        "wrong fragment span changed the page");
    bytes.reserve(bytes.size() * 2);
    allocation_budget = 0;
    const auto failed = codec::apply_edit(bytes, plan);
    allocation_budget = -1;
    require(failed == status::out_of_memory && bytes == original,
        "fragment shrink failure changed the page");
    require(codec::apply_edit(bytes, plan) == status::ok && bytes == reference.bytes,
        "native fragment pop changed the surviving encoding");
    pkv::list_page_edit fallback;
    require(codec::pop_end(bytes, false, 4097, update, &fallback) == status::ok &&
        !update.applied && !fallback.id, "cross-page fragment did not defer");
}

static void test_encoded_pop_failures() {
    list_page page;
    page.id = 7;
    page.fragments = {fragment(std::string(100, 'x')), fragment("")};
    std::string bytes;
    require(codec::encode_page(page, bytes) == status::ok, "pop fixture");
    for (size_t i = 0; i < bytes.size(); ++i) {
        list_page_update update;
        update.id = 999; update.bytes = "sentinel"; update.value = "value";
        require(codec::pop_end(bytes.substr(0, i), true, 1024, update) ==
            status::invalid && update.id == 999 &&
            update.bytes == "sentinel" && update.value == "value",
            "truncated pop changed output");
    }
    for (long budget = 0; budget < 2; ++budget) {
        list_page_update update;
        update.id = 999;
        allocation_budget = budget;
        const auto result = codec::pop_end(bytes, true, 1024, update);
        allocation_budget = -1;
        require(result == status::out_of_memory && update.id == 999 &&
            update.bytes.empty() && update.value.empty(),
            "failed pop allocation changed output");
    }
    list_page_update update;
    update.id = 999;
    require(codec::pop_end(bytes, true, 99, update) == status::invalid &&
        update.id == 999, "pop bypassed member bound");
    bytes[52] = 1;
    require(codec::pop_end(bytes, false, 1024, update) == status::invalid &&
        update.id == 999, "pop bypassed remote fragment validation");
    page.fragments = {fragment("only")};
    require(codec::encode_page(page, bytes) == status::ok &&
        codec::pop_end(bytes, true, 1024, update) == status::ok &&
        !update.applied, "last fragment did not request fallback");
    page.next = 8;
    page.fragments = {fragment("a", 0, 2), fragment("b", 1, 2)};
    require(codec::encode_page(page, bytes) == status::ok &&
        codec::pop_end(bytes, true, 1024, update) == status::ok &&
        !update.applied, "fragmented member did not request fallback");
}

static void test_fill_target() {
    list_page page;
    page.id = 7;
    page.fragments.assign(74, fragment("abc"));
    std::string bytes;
    require(codec::encode_page(page, bytes) == status::ok,
        "small fill target fixture");
    for (bool left : {false, true}) {
        list_page_update small, normal;
        require(codec::push_end(bytes, {"new"}, left, 10, small, 2048) ==
            status::ok && !small.applied && small.id == 7,
            "small fill target ignored");
        require(codec::push_end(bytes, {"new"}, left, 10, normal) ==
            status::ok && normal.applied, "format bound was reduced");
    }
    page.fragments.assign(600, fragment("abc"));
    require(codec::encode_page(page, bytes) == status::ok,
        "wide legacy page fixture");
    list_page_update small, normal;
    require(codec::push_end(bytes, {"new"}, false, 10, small, 2048) ==
        status::ok && small.applied && codec::push_end(bytes, {"new"}, false,
            10, normal) == status::ok && small.bytes == normal.bytes,
        "wide legacy page lost free capacity");
    for (const auto limit : {size_t(55), codec::max_page_bytes + 1}) {
        list_page_update out;
        out.id = 999; out.bytes = "sentinel";
        require(codec::push_end(bytes, {"new"}, false, 10, out, limit) ==
            status::invalid && out.id == 999 && out.bytes == "sentinel",
            "invalid fill target changed output");
    }
}

static void test_root_buffer_reuse() {
    for (uint64_t last : {uint64_t(0), uint64_t(3)}) {
        list_root root;
        root.elements = 10; root.head = 1; root.tail = 2; root.last_id = last;
        std::string expected, out = "keep";
        require(codec::encode_root(root, expected) == status::ok,
            "root reuse fixture");
        out.reserve(48);
        allocation_budget = 0;
        const auto result = codec::encode_root(root, out);
        allocation_budget = -1;
        require(result == status::ok && out == expected,
            "root encode did not reuse output capacity");
        root.expires_at_ms = -2;
        require(codec::encode_root(root, out) == status::invalid &&
            out == expected, "invalid root changed reused output");
        root.expires_at_ms = -1;
        std::string cold = "keep";
        allocation_budget = 0;
        const auto failed = codec::encode_root(root, cold);
        allocation_budget = -1;
        require(failed == status::out_of_memory && cold == "keep",
            "root allocation failure changed output");
    }
}

struct validation_view { unsigned value; };
struct allocation_view { unsigned value; };
struct late_view { unsigned value; };
struct deferred_view { unsigned value; };

static void test_validation_workspace() {
    using workspace = detail::validated_page_cache<validation_view>;
    const std::string bytes("a\0binary", 8);
    {
        using deferred = detail::validated_page_cache<deferred_view>;
        deferred cache(false);
        deferred_view out{999};
        cache.remember(bytes, 10, deferred_view{3});
        require(!cache.available() && !cache.lookup(bytes, 10, out) &&
            out.value == 999 && deferred::retained_capacity() == 0,
            "disabled validation cache acquired storage");
        allocation_budget = 0;
        cache.activate();
        allocation_budget = -1;
        cache.remember(bytes, 10, deferred_view{3});
        cache.activate();
        require(cache.available() && cache.lookup(bytes, 10, out) &&
            out.value == 3, "deferred validation cache activation");
    }
    {
        workspace cache;
        validation_view value{17}, out{999};
        cache.remember(bytes, 10, value);
        require(cache.lookup(bytes, 10, out) && out.value == 17 &&
            cache.lookup(bytes, 11, out), "validation cache bound reuse");
        out.value = 999;
        require(!cache.lookup(bytes, 9, out) && out.value == 999 &&
            !cache.lookup(std::string("a\0Binary", 8), 10, out),
            "validation cache ignored bytes or stricter bound");
        workspace nested;
        require(!nested.available() && !nested.lookup(bytes, 10, out),
            "nested validation cache reused live lease");
        nested.remember("other", 10, validation_view{19});
        require(cache.lookup(bytes, 10, out) && out.value == 17,
            "nested validation cache changed parent");
    }
    {
        workspace cache;
        validation_view out{};
        require(cache.lookup(bytes, 10, out) && out.value == 17 &&
            workspace::retained_capacity() <= workspace::max_capacity,
            "validation cache lifetime/capacity");
        cache.remember(std::string(workspace::max_bytes + 1, 'x'), 10,
            validation_view{3});
        require(cache.lookup(bytes, 10, out), "oversized cache entry retained");
    }
    using fault_workspace = detail::validated_page_cache<allocation_view>;
    fault_workspace cache;
    allocation_view out{999};
    allocation_budget = 0;
    cache.remember(bytes, 10, allocation_view{7});
    allocation_budget = -1;
    require(!cache.lookup(bytes, 10, out) && out.value == 999 &&
        fault_workspace::retained_capacity() <= std::string().capacity(),
        "optional cache allocation failure escaped or published");
    cache.remember(bytes, 10, allocation_view{7});
    require(cache.lookup(bytes, 10, out) && out.value == 7,
        "validation cache did not recover after allocation failure");
}

static void test_validation_threads_and_shutdown() {
    std::vector<std::thread> workers;
    for (unsigned id = 0; id < 4; ++id) workers.emplace_back([id] {
        using workspace = detail::validated_page_cache<validation_view>;
        workspace cache;
        const auto bytes = std::to_string(id);
        for (unsigned i = 0; i < 100; ++i) {
            cache.remember(bytes, 10, validation_view{id});
            validation_view out{999};
            require(cache.lookup(bytes, 10, out) && out.value == id,
                "validation cache shared between threads");
        }
    });
    for (auto& worker : workers) worker.join();
    bool retired = false;
    struct late_probe {
        bool* retired;
        ~late_probe() {
            using workspace = detail::validated_page_cache<late_view>;
            workspace cache;
            *retired = !cache.available() &&
                workspace::retained_capacity() == 0;
        }
    };
    std::thread last([&] {
        static thread_local late_probe probe{&retired};
        (void)probe;
        detail::validated_page_cache<late_view> cache;
        cache.remember("late", 10, late_view{1});
    });
    last.join();
    require(retired, "late TLS validation cache reopened destroyed storage");
}

static void test_compact_validation_reuse() {
    for (bool left : {false, true}) {
        list_page page;
        page.id = 7;
        for (unsigned i = 0; i < 8; ++i)
            page.fragments.push_back(fragment("a"));
        std::string bytes;
        require(codec::encode_packed_page(page, bytes) == status::ok,
            "cached compact fixture");
        list_page_update update;
        require(codec::push_end(bytes, {"longer"}, left, 8, update) ==
            status::ok && update.applied, "cached compact push");
        const auto good = update.bytes;
        rejected_push(good, {"x"}, left, 5, status::invalid);
        for (unsigned damage = 0; damage < 5; ++damage) {
            auto bad = good;
            if (damage == 0) std::fill(bad.begin() + 4, bad.begin() + 8, 0);
            if (damage == 1) std::fill(bad.begin() + 8, bad.begin() + 16, 0);
            if (damage == 2)
                std::fill(bad.begin() + 32, bad.begin() + 36, '\xff');
            if (damage == 3) bad.pop_back();
            if (damage == 4) bad.push_back('\0');
            rejected_push(bad, {"x"}, left, 8, status::invalid);
        }
        list_page expected;
        require(codec::decode_page(good, expected) == status::ok,
            "cached compact decode");
        if (left) expected.fragments.insert(expected.fragments.begin(),
            fragment("last"));
        else expected.fragments.push_back(fragment("last"));
        std::string encoded;
        require(codec::encode_packed_page(expected, encoded) == status::ok &&
            codec::push_end(good, {"last"}, left, 8, update) == status::ok &&
            update.applied && update.bytes == encoded,
            "cached compact bytes/order after rejected edits");
    }
}

static void test_compact_length_width() {
    list_page page;
    page.id = 1;
    const std::string narrow(255, '\xff'), wide(256, '\0');
    page.fragments = {fragment(""), fragment(narrow)};
    std::string bytes;
    require(codec::encode_packed_page(page, bytes) == status::ok &&
        bytes[3] == 3 && bytes.size() == codec::page_header_bytes + 2 + 255,
        "short packed members use byte lengths");
    list_page_update update;
    uint32_t count = 0;
    require(codec::index_compact(bytes, 0, true, 256, count, update) ==
        status::ok && count == 2 && update.value == narrow,
        "read byte length at its upper bound");
    require(codec::index_compact(bytes, 0, true, 256, count, update, &wide) ==
        status::ok && count == 2 && !update.applied,
        "wide replacement requests decoded fallback");
    require(codec::push_end(bytes, {wide}, false, 256, update) ==
        status::ok && !update.applied && !update.linked,
        "wide append requests decoded fallback");
    require(codec::pop_end(bytes, false, 256, update) == status::ok &&
        update.applied && update.value == narrow && update.bytes[3] == 3,
        "right pop preserves byte-length encoding");
    bytes.pop_back();
    list_page decoded;
    require(codec::decode_page(bytes, decoded) == status::invalid,
        "truncated byte-length payload rejected");
    page.fragments.push_back(fragment(wide));
    require(codec::encode_packed_page(page, bytes) == status::ok && bytes[3] == 2 &&
        codec::decode_page(bytes, decoded) == status::ok &&
        decoded.fragments.size() == 3 && decoded.fragments[1].bytes == narrow &&
        decoded.fragments[2].bytes == wide, "wide packed pages still round-trip");
}

static void test_native_append_plan() {
    list_page page;
    page.id = 0x0102030405060708ULL;
    page.previous = 7;
    for (unsigned i = 0; i < 8; ++i) {
        list_fragment item;
        item.bytes.assign(i % 4, static_cast<char>(i));
        item.total_bytes = item.bytes.size();
        page.fragments.push_back(std::move(item));
    }
    std::string bytes;
    require(codec::encode_packed_page(page, bytes) == status::ok, "append seed");
    {
        pkv::list_page_edit cold;
        list_page_update update;
        require(codec::plan_append(bytes, "prime", 512, codec::max_page_bytes,
            update, cold, true) == status::ok && !cold.id && !update.applied,
            "cold append must defer without changing the page");
        require(codec::push_end(bytes, {"prime"}, false, 512, update) ==
            status::ok && update.applied, "prime complete writer cache");
        bytes = std::move(update.bytes);
        pkv::list_page_edit hot;
        require(codec::plan_append(bytes, "next", 512, codec::max_page_bytes,
            update, hot, true) == status::ok && hot.id == page.id,
            "validated hot page must allow native append");
        auto damaged = bytes;
        damaged[codec::page_header_bytes] = '\xff';
        pkv::list_page_edit rejected;
        require(codec::plan_append(damaged, "next", 512, codec::max_page_bytes,
            update, rejected, true) == status::ok && !rejected.id,
            "changed page must defer to the full writer");
        require(codec::push_end(damaged, {"next"}, false, 512, update) ==
            status::invalid, "cold fallback must still reject corrupt bytes");
    }
    for (unsigned step = 0; step < 128; ++step) {
        const std::string value(step % 20, static_cast<char>(step));
        pkv::list_page_edit plan;
        list_page_update update, ordinary;
        require(codec::plan_append(bytes, value, 512, codec::max_page_bytes,
            update, plan) == status::ok && plan.id == page.id,
            "native append planning");
        require(codec::push_end(bytes, {value}, false, 512, ordinary) == status::ok &&
            ordinary.applied, "append reference writer");
        const auto original = bytes;
        for (unsigned bad = 0; bad < 4; ++bad) {
            auto invalid = plan;
            if (bad == 0) ++invalid.expected_bytes;
            if (bad == 1) ++invalid.expected_count;
            if (bad == 2) ++invalid.id;
            if (bad == 3) invalid.suffix[0] ^= 0x40;
            require(codec::apply_append(bytes, invalid) == status::invalid &&
                bytes == original, "bad append changed stored page");
        }
        auto damaged = bytes;
        damaged[codec::page_header_bytes] = '\xff';
        pkv::list_page_edit rejected;
        require(codec::plan_append(damaged, value, 512, codec::max_page_bytes,
            update, rejected) == status::invalid && !rejected.id,
            "append skipped full byte validation");
        require(codec::apply_append(bytes, plan) == status::ok &&
            bytes == ordinary.bytes, "append bytes differ from full writer");
    }
    // Force an allocation boundary; failure must preserve every logical byte.
    bool saw_failure = false;
    for (unsigned count = 1; count < 20 && !saw_failure; ++count) {
        page.fragments.resize(count);
        for (auto& item : page.fragments) { item.bytes.clear(); item.total_bytes = 0; }
        std::string source;
        require(codec::encode_packed_page(page, source) == status::ok, "append OOM seed");
        std::string cold(source.data(), source.size());
        pkv::list_page_edit plan;
        list_page_update update;
        require(codec::plan_append(cold, std::string(19, 'z'), 512,
            codec::max_page_bytes, update, plan) == status::ok && plan.id,
            "append OOM plan");
        if (cold.capacity() >= cold.size() + plan.suffix.size()) continue;
        allocation_budget = 0;
        const auto result = codec::apply_append(cold, plan);
        allocation_budget = -1;
        require(result == status::out_of_memory && cold == source,
            "append reserve failure published bytes");
        saw_failure = true;
    }
    require(saw_failure, "append allocation failure not exercised");
}

static void test_native_wide_push() {
    list_page page;
    page.id = 0x564321;
    const size_t widths[] = {255, 256, 1024, 4096, 4097, codec::wide_member_bytes};
    for (size_t width : widths) {
        const auto count = width == codec::wide_member_bytes ? 1 : width >= 4096 ? 2 : 8;
        page.fragments.assign(count, fragment(std::string(width, 'x')));
        std::string source;
        require(codec::encode_packed_page(page, source) == status::ok &&
            source.size() > 2048, "wide push seed exceeds validation cache");
        const std::string value(width, '\0');
        for (bool left : {false, true}) {
            pkv::list_page_edit plan;
            list_page_update update;
            require(codec::plan_push(source, value, left, width,
                codec::max_page_bytes, update, plan, true) == status::ok && !plan.id,
                "wide push must miss the validation cache");
            require(codec::plan_push(source, value, left, width,
                codec::max_page_bytes, update, plan) == status::ok && plan.id == page.id,
                "wide push can plan from validated page bytes");
            require(codec::push_end(source, {value}, left, width, update) == status::ok &&
                update.applied, "wide push reference encoding");
            require(plan.length_bytes == (width <= 255 ? 1 : 4),
                "native push length encoding");
            auto stored = source;
            auto invalid = plan;
            invalid.length_bytes = plan.length_bytes == 1 ? 4 : 1;
            require(codec::apply_edit(stored, invalid) == status::invalid &&
                stored == source, "mismatched push width changed bytes");
            invalid = plan;
            invalid.suffix.resize(plan.length_bytes - 1);
            require(codec::apply_edit(stored, invalid) == status::invalid &&
                stored == source, "truncated push length changed bytes");
            require(codec::apply_edit(stored, plan) == status::ok &&
                stored == update.bytes, "owned wide push differs from writer");
            plan.suffix.clear();
            plan.borrowed_value = &value;
            const bool planned = width == 256 || width == 1024;
            if (planned) plan.fill_limit = codec::max_page_bytes;
            stored = std::string(source.data(), source.size());
            require(stored.capacity() < stored.size() + plan.added_bytes(),
                "wide push reserve failure requires allocation");
            allocation_budget = 0;
            const auto failed = codec::apply_edit(stored, plan);
            allocation_budget = -1;
            require(failed == status::out_of_memory && stored == source,
                "borrowed wide push reserve failure changed bytes");
            require(codec::apply_edit(stored, plan) == status::ok &&
                stored == update.bytes, "borrowed wide push differs from writer");
            if (planned) {
                const auto members = (codec::max_page_bytes - codec::page_header_bytes) /
                    (codec::fragment_header_bytes + width);
                require(stored.capacity() >= codec::page_header_bytes + members * (4 + width),
                    "wide push did not reserve planned members");
            }
        }
    }
}

static void test_native_right_pop() {
    for (size_t width : {72, 512, 1024, 4096, 4097, 5400}) {
        list_page page;
        page.id = 21;
        page.previous = 20;
        for (char byte = 'a'; byte <= (width >= 4096 ? 'c' : 'd'); ++byte)
            page.fragments.push_back(fragment(std::string(width, byte)));
        std::string bytes;
        require(codec::encode_packed_page(page, bytes) == status::ok, "right pop seed");
        bytes.reserve(bytes.size() * 2);
        const auto capacity = bytes.capacity();
        const auto original = bytes;
        list_page_update update, reference;
        pkv::list_page_edit plan;
        update.value.reserve(width);
        const auto* reply_buffer = update.value.data();
        allocation_budget = 0;
        const auto planned = codec::plan_uniform_pop_right(bytes, width, 0, update, plan);
        allocation_budget = -1;
        require(planned == status::ok && update.value.data() == reply_buffer &&
            plan.id == page.id && update.value == page.fragments.back().bytes,
            "right pop plan must reuse the reply buffer and return the final member");
        require(codec::pop_end(bytes, false, width, reference) == status::ok && reference.applied,
            "right pop reference encoding");
        require(plan.length_bytes == (width <= 255 ? 1 : 4), "right pop length encoding");
        auto stale = plan;
        stale.length_bytes = plan.length_bytes == 1 ? 4 : 1;
        require(codec::apply_edit(bytes, stale) == status::invalid && bytes == original,
            "wrong right pop length encoding changed the page");
        stale = plan;
        ++stale.expected_count;
        require(codec::apply_edit(bytes, stale) == status::invalid && bytes == original,
            "stale right pop plan changed the page");
        allocation_budget = 0;
        const auto failed = codec::apply_edit(bytes, plan);
        allocation_budget = -1;
        require(failed == status::out_of_memory && bytes == original,
            "right pop shrink failure changed the page");
        require(codec::apply_edit(bytes, plan) == status::ok && bytes == reference.bytes &&
            bytes.capacity() < capacity, "right pop must publish a smaller allocation");
        pkv::list_page_edit next;
        const auto count = static_cast<uint16_t>(page.fragments.size() - 1);
        require(codec::plan_uniform_pop_right(bytes, width, count, update, next) == status::ok &&
            next.id == page.id && update.value == page.fragments[count - 1].bytes,
            "right pop should reuse the committed count");

        page.fragments.back() = fragment("mixed");
        require(codec::encode_packed_page(page, bytes) == status::ok, "mixed right pop seed");
        pkv::list_page_edit mixed;
        require(codec::plan_uniform_pop_right(bytes, width, 0, update, mixed) == status::ok &&
            !mixed.id, "mixed width page must use the ordinary writer");
    }
}

static void test_native_push_both_ends() {
    list_page page;
    page.id = 0x334455667788ULL;
    std::vector<std::string> model(8, "seed");
    for (const auto& value : model) page.fragments.push_back(fragment(value));
    std::string bytes;
    require(codec::encode_packed_page(page, bytes) == status::ok, "push seed");
    list_page_update update;
    require(codec::push_end(bytes, {"warm"}, false, 512, update) == status::ok &&
        update.applied, "push validation prime");
    bytes = std::move(update.bytes);
    model.push_back("warm");
    for (unsigned step = 0; step < 180; ++step) {
        const bool left = step % 3 != 0;
        const std::string value(step % 20, static_cast<char>(step));
        pkv::list_page_edit plan;
        require(codec::plan_push(bytes, value, left, 512, codec::max_page_bytes,
            update, plan, true) == status::ok && plan.id == page.id &&
            plan.left == left, "cached endpoint push planning");
        list_page_update reference;
        require(codec::push_end(bytes, {value}, left, 512, reference) ==
            status::ok && reference.applied, "push reference encoding");
        const auto original = bytes;
        for (unsigned bad = 0; bad < 4; ++bad) {
            auto invalid = plan;
            if (bad == 0) ++invalid.expected_bytes;
            if (bad == 1) ++invalid.expected_count;
            if (bad == 2) ++invalid.id;
            if (bad == 3) invalid.suffix[0] ^= 0x40;
            require(codec::apply_edit(bytes, invalid) == status::invalid &&
                bytes == original, "invalid push changed bytes");
        }
        require(codec::apply_edit(bytes, plan) == status::ok &&
            bytes == reference.bytes, "native endpoint push differs from writer");
        if (left) model.insert(model.begin(), value);
        else model.push_back(value);
        list_page decoded;
        require(codec::decode_page(bytes, decoded) == status::ok &&
            decoded.fragments.size() == model.size(), "push decode count");
        for (size_t i = 0; i < model.size(); ++i)
            require(decoded.fragments[i].bytes == model[i], "push decode model");
        if (step % 16 == 0) {
            list_page_update popped, restored;
            require(codec::pop_end(bytes, false, 512, popped) == status::ok &&
                popped.applied && popped.value == model.back(),
                "prepend cached last-fragment position");
            require(codec::push_end(popped.bytes, {popped.value}, false, 512,
                restored) == status::ok && restored.applied &&
                restored.bytes == bytes, "restore right endpoint validation");
        }
    }
    for (bool left : {false, true}) {
        page.fragments.assign(1, fragment(""));
        require(codec::encode_packed_page(page, bytes) == status::ok,
            "push reserve failure seed");
        std::string cold(bytes.data(), bytes.size());
        pkv::list_page_edit plan;
        require(codec::plan_push(cold, std::string(19, 'x'), left, 512,
            codec::max_page_bytes, update, plan) == status::ok && plan.id,
            "push reserve failure plan");
        require(cold.capacity() < cold.size() + plan.suffix.size(),
            "push reserve failure requires allocation");
        allocation_budget = 0;
        const auto result = codec::apply_edit(cold, plan);
        allocation_budget = -1;
        require(result == status::out_of_memory && cold == bytes,
            "push reserve failure changed bytes");
    }
    page.previous = 17;
    require(codec::encode_packed_page(page, bytes) == status::ok, "linked push seed");
    pkv::list_page_edit rejected;
    require(codec::plan_push(bytes, "x", true, 512, codec::max_page_bytes,
        update, rejected) == status::invalid && !rejected.id,
        "left push accepted non-endpoint page");
}

int main() {
    test_native_push_both_ends();
    test_native_wide_push();
    test_native_right_pop();
    test_native_append_plan();
    test_validation_workspace();
    test_validation_threads_and_shutdown();
    test_compact_validation_reuse();
    test_compact_length_width();
    test_root_buffer_reuse();
    test_fill_target();
    test_complete_after_unfinished_fragment();
    test_root();
    test_extended_root();
    test_page();
    test_corruption();
    test_fragmented_member();
    test_allocation_failure();
    test_encoded_push();
    test_encoded_push_failures();
    test_encoded_pop();
    test_native_fragment_pop();
    test_encoded_pop_failures();
    std::cout << "Bounded List page codec tests passed\n";
}
