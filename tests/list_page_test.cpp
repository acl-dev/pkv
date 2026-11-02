#include "coder/list_page.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <random>

// Sweep every allocation boundary, including fragment payload allocation.
static long allocation_budget = -1;
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
    encoded[3] = 2;
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

int main() {
    test_root();
    test_extended_root();
    test_page();
    test_corruption();
    test_fragmented_member();
    test_allocation_failure();
    std::cout << "Bounded List page codec tests passed\n";
}
