#include "string_digest.h"
#include <cstdint>
#include <new>
#ifdef PKV_XXHASH_NAMESPACED
#define XXH_NAMESPACE PKV_
#endif
#include <xxhash.h>
#if XXH_VERSION_NUMBER < 800
#error "String digests require xxHash 0.8.0 or newer"
#endif

namespace pkv {
namespace {
std::string hex_digest(uint64_t value) {
    static const char digits[] = "0123456789abcdef";
    char out[16];
    for (unsigned i = 0; i < 16; ++i) {
        out[15 - i] = digits[value & 15]; value >>= 4;
    }
    return std::string(out, sizeof(out));
}
} // namespace
string_digest::string_digest() {
    auto* state = XXH3_createState();
    if (!state) throw std::bad_alloc();
    XXH3_64bits_reset(state);
    state_ = state;
}
string_digest::~string_digest() {
    XXH3_freeState(static_cast<XXH3_state_t*>(state_));
}
void string_digest::update(const std::string& bytes) {
    if (!bytes.empty()) XXH3_64bits_update(
        static_cast<XXH3_state_t*>(state_), bytes.data(), bytes.size());
}
std::string string_digest::finish() const {
    return hex_digest(XXH3_64bits_digest(
        static_cast<const XXH3_state_t*>(state_)));
}
std::string string_digest::value(const std::string& bytes) {
    return hex_digest(XXH3_64bits(bytes.empty() ? "" : bytes.data(),
        bytes.size()));
}
bool string_digest::equal(const std::string& expected,
    const std::string& actual) {
    if (expected.size() != 16 || actual.size() != 16) return false;
    for (size_t i = 0; i < 16; ++i) {
        auto ch = static_cast<unsigned char>(expected[i]);
        if (ch >= 'A' && ch <= 'F') ch += 'a' - 'A';
        if (ch != static_cast<unsigned char>(actual[i])) return false;
    }
    return true;
}
} // namespace pkv
