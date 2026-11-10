#include "coder/packed_hash.h"
#include <cstdio>
#include <cstdlib>
#include <new>
#include <climits>
static bool fail_next = false;
void* operator new(size_t n) { if (fail_next) { fail_next = false; throw std::bad_alloc(); } if (void* p = std::malloc(n ? n : 1)) return p; throw std::bad_alloc(); }
void* operator new[](size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
#if __cplusplus >= 201402L
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }
#endif
#define REQUIRE(x) do { if (!(x)) { std::fprintf(stderr,"failed line %d\n",__LINE__); std::abort(); } } while (0)
using namespace pkv;
namespace packed = packed_hash;
static bool equal(const packed::root& a, const packed::root& b) {
    return a.bits == b.bits && a.elements == b.elements &&
        a.expires_at_ms == b.expires_at_ms && a.sizes == b.sizes;
}
static void rejected(const std::string& bytes) {
    packed::root marker; marker.bits = 6; marker.elements = 99;
    marker.expires_at_ms = 123; marker.sizes[33] = 3;
    const auto before = marker;
    REQUIRE(packed::decode_root(bytes, marker) != map_page_status::ok);
    REQUIRE(equal(marker, before));
}
static void roots() {
    for (unsigned bits : {4u, 5u, 6u}) for (unsigned count : {1u,63u,64u,129u,256u,512u}) {
        packed::root value; value.bits = bits; value.elements = uint16_t(count);
        for (unsigned i = 0; i < count; ++i) ++value.sizes[i % value.groups()];
        std::string bytes = "unchanged";
        if (count > value.groups() * packed::max_group_fields) {
            REQUIRE(packed::encode_root(value, bytes) == map_page_status::invalid && bytes == "unchanged");
            continue;
        }
        for (int64_t ttl : {int64_t(-1),int64_t(0),int64_t(4102444800123LL),int64_t(INT64_MAX)}) {
            value.expires_at_ms = ttl;
            REQUIRE(packed::encode_root(value, bytes) == map_page_status::ok);
            REQUIRE(packed::matches(bytes) && bytes.size() <= packed::max_root_bytes);
            packed::root decoded;
            REQUIRE(packed::decode_root(bytes, decoded) == map_page_status::ok && equal(decoded, value));
            for (size_t cut = 0; cut < bytes.size(); ++cut) rejected(bytes.substr(0, cut));
            rejected(bytes + "x");
            auto bad = bytes; bad[4] = char(0x40 | unsigned(bad[4])); rejected(bad);
            bad = bytes; bad[4] = char((unsigned(bad[4]) & 0x80) | 7); rejected(bad);
            bad = bytes; bad[5] = char(3); rejected(bad);
            bad = bytes; bad.back() = char(17); rejected(bad);
            if (ttl >= 0) { bad = bytes; bad[7] = char(0x80); rejected(bad); }
            // Every accepted single-byte mutation must re-encode canonically.
            for (size_t i = 4; i < bytes.size(); ++i) {
                auto changed = bytes; changed[i] ^= 1;
                if (packed::decode_root(changed, decoded) == map_page_status::ok) {
                    std::string encoded; REQUIRE(packed::encode_root(decoded, encoded) == map_page_status::ok && encoded == changed);
                } else rejected(changed);
            }
        }
    }
    packed::root good; good.elements = 1; good.sizes[0] = 1;
    for (unsigned kind = 0; kind < 5; ++kind) {
        auto invalid = good;
        if (kind == 0) invalid.bits = UINT_MAX;
        if (kind == 1) invalid.expires_at_ms = -2;
        if (kind == 2) invalid.elements = 0;
        if (kind == 3) invalid.sizes[16] = 1;
        if (kind == 4) invalid.sizes[0] = 2;
        std::string out = "unchanged";
        REQUIRE(packed::encode_root(invalid, out) == map_page_status::invalid && out == "unchanged");
    }
    std::string out = "unchanged"; fail_next = true;
    REQUIRE(packed::encode_root(good, out) == map_page_status::out_of_memory && out == "unchanged" && !fail_next);
    REQUIRE(packed::encode_root(good,out) == map_page_status::ok);
    fail_next = true; const auto fixed_result = packed::encode_root(good,out);
    const bool no_allocation = fail_next; fail_next = false;
    REQUIRE(fixed_result == map_page_status::ok && no_allocation);
    packed::root marker;
    REQUIRE(packed::decode_root(std::string("PMR\7",4), marker) == map_page_status::unsupported);
    REQUIRE(packed::creation_bits(63) == 0 && packed::creation_bits(64) == 4 &&
        packed::creation_bits(129) == 5 && packed::creation_bits(512) == 6 && packed::creation_bits(513) == 0);
    REQUIRE(packed::bucket("", 0, UINT_MAX) == packed::max_groups);
    uint32_t random = 76;
    for (unsigned trial = 0; trial < 10000; ++trial) {
        random = random * 1664525u + 1013904223u;
        std::string fuzz(random % 96, '\0');
        for (auto& c : fuzz) { random = random * 1664525u + 1013904223u; c = char(random >> 24); }
        if (fuzz.size() >= 4) fuzz.replace(0, 4, "PMR\14", 4);
        packed::root result;
        if (packed::decode_root(fuzz, result) != map_page_status::ok) rejected(fuzz);
    }
}
template<class F> static void invalid_page(F fn) {
    bool threw = false; try { fn(); } catch (const std::runtime_error&) { threw = true; } REQUIRE(threw);
}
static void pages() {
    for (unsigned bits : {4u,5u,6u}) {
        const unsigned group = 3;
        std::map<std::string,std::string> fields;
        std::string extra;
        for (unsigned i = 0; extra.empty(); ++i) {
            char s[32]; std::snprintf(s,sizeof(s),"f:%u",i);
            if (packed::bucket(s,bits) != group) continue;
            if (fields.size() < 16) fields[s] = std::string("v\0x",3); else extra = s;
        }
        packed::root owner; owner.bits = bits; owner.elements = 16; owner.sizes[group] = 16;
        auto bytes = compact_hash::encode(fields);
        packed::page_view page(bytes, owner, group);
        unsigned i = 0;
        for (const auto& field : fields) { auto item = page.at(i++); REQUIRE(item.first.str() == field.first && item.second.str() == field.second); }
        bool added = true; const auto before = bytes;
        REQUIRE(!page.put(extra,std::string("new"),bytes,added) && bytes == before && added);
        REQUIRE(!page.put(fields.begin()->first,std::string(65,'x'),bytes,added) && bytes == before);
        REQUIRE(!page.put(std::string("different-route"),std::string("new"),bytes,added) && bytes == before);
        REQUIRE(page.put(fields.begin()->first,std::string(64,'x'),bytes,added) && !added);
        std::string found; REQUIRE(packed::page_view(bytes, owner, group).get(fields.begin()->first,found) && found == std::string(64,'x'));
        REQUIRE(packed::page_view(bytes, owner, group).put(fields.begin()->first,std::string(),bytes,added) && !added);
        auto remaining = packed::page_view(bytes, owner, group).without(uint16_t(1 | (1u<<15)));
        auto reduced = owner; reduced.elements = 14; reduced.sizes[group] = 14;
        REQUIRE(packed::page_view(remaining,reduced,group).count() == 14);
        REQUIRE(packed::page_view(bytes,owner,group).without(UINT16_MAX).empty());
        invalid_page([&] { packed::page_view(bytes,owner,group).at(16); });
        auto wrong = owner; wrong.sizes[group] = 0; wrong.sizes[group+1] = 16;
        invalid_page([&] { packed::page_view bad(bytes,wrong,group+1); });
        const auto expiring = compact_hash::encode(fields,1);
        invalid_page([&] { packed::page_view bad(expiring,owner,group); });
    }
    for (const auto& field : {std::string(),std::string("a\0b",3)}) {
        packed::root owner; owner.elements = 1; const auto group = packed::bucket(field,owner.bits); owner.sizes[group] = 1;
        auto bytes = compact_hash::encode({{field,std::string()}});
        REQUIRE(packed::page_view(bytes,owner,group).at(0).first.str() == field);
        invalid_page([&] { packed::page_view(bytes,owner,group).without(2); });
    }
}
int main() { roots(); pages(); std::puts("packed Hash codec checks passed"); }
