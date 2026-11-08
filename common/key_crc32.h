#pragma once
#include <cstddef>
#include <cstdint>
#include <acl-lib/acl/stdlib/acl_hash.h>

namespace pkv {

// Preserve ACL's IEEE CRC32 routing, including its null-input convention.
// Only targets compiled with ARM CRC support use the instruction path.
#if defined(__aarch64__) && defined(__ARM_FEATURE_CRC32) && \
    defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
uint32_t key_crc32(const void* bytes, size_t size);
#else
inline uint32_t key_crc32(const void* bytes, size_t size) {
    return acl_hash_crc32(bytes, size);
}
#endif

} // namespace pkv
