#include "key_crc32.h"
#if defined(__aarch64__) && defined(__ARM_FEATURE_CRC32) && \
    defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#include <arm_acle.h>
#include <cstring>

namespace pkv {

// Keep the loop out of routing callers, including under link-time optimization.
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
uint32_t key_crc32(const void* bytes, size_t size) {
    if (!bytes) return 0;
    const auto* cursor = static_cast<const unsigned char*>(bytes);
    uint32_t crc = UINT32_MAX;
    while (size >= 8) {
        uint64_t word;
        std::memcpy(&word, cursor, sizeof(word));
        crc = __crc32d(crc, word);
        cursor += 8; size -= 8;
    }
    if (size >= 4) {
        uint32_t word;
        std::memcpy(&word, cursor, sizeof(word));
        crc = __crc32w(crc, word);
        cursor += 4; size -= 4;
    }
    if (size >= 2) {
        uint16_t word;
        std::memcpy(&word, cursor, sizeof(word));
        crc = __crc32h(crc, word);
        cursor += 2; size -= 2;
    }
    if (size) crc = __crc32b(crc, *cursor);
    return ~crc;
}

} // namespace pkv
#endif
