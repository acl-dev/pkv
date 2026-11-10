#include "common/key_crc32.h"
#include <cstdlib>
#include <iostream>
#include <vector>
#ifndef _WIN32
#include <sys/mman.h>
#include <unistd.h>
#endif

static void require(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}

static void compare(const void* data, size_t size) {
    require(pkv::key_crc32(data, size) == acl_hash_crc32(data, size),
        "CRC32 changed a routing result");
}

int main() {
    require(pkv::key_crc32("123456789", 9) == 0xcbf43926U,
        "IEEE CRC32 known vector");
    require(pkv::key_crc32("", 0) == 0, "empty CRC32");
    compare(nullptr, 0); compare(nullptr, 19);
    std::vector<unsigned char> bytes(65536 + 16);
    uint32_t random = 76;
    for (auto& byte : bytes) {
        random ^= random << 13; random ^= random >> 17; random ^= random << 5;
        byte = static_cast<unsigned char>(random);
    }
    for (size_t offset = 0; offset < 16; ++offset) {
        for (size_t size = 0; size <= 257; ++size)
            compare(bytes.data() + offset, size);
        for (size_t size : {511U, 512U, 513U, 1023U, 1024U, 1025U,
                4095U, 4096U, 4097U, 65535U, 65536U})
            compare(bytes.data() + offset, size);
    }
    for (size_t i = 0; i < 512; ++i) {
        random ^= random << 13; random ^= random >> 17; random ^= random << 5;
        const size_t size = random % 65537;
        compare(bytes.data() + (random >> 16) % 16, size);
    }
#ifndef _WIN32
    const long length = sysconf(_SC_PAGESIZE);
    require(length > 257, "page size");
    const auto page = static_cast<size_t>(length);
    void* allocation = mmap(nullptr, page * 2, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANON, -1, 0);
    require(allocation != MAP_FAILED, "guarded CRC32 allocation");
    auto* end = static_cast<unsigned char*>(allocation) + page;
    for (size_t i = 0; i < page; ++i)
        static_cast<unsigned char*>(allocation)[i] = static_cast<unsigned char>(i);
    require(mprotect(end, page, PROT_NONE) == 0, "CRC32 guard page");
    for (size_t size = 0; size <= 257; ++size) compare(end - size, size);
    compare(end - page, page); compare(end - page + 1, page - 1);
    require(munmap(allocation, page * 2) == 0, "guarded CRC32 release");
#endif
    std::cout << "CRC32 routing equivalence passed\n";
}
