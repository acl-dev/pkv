#pragma once
#include <acl-lib/acl/lib_acl.h>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace pkv {

namespace disk_address {
inline void number(std::string& out, uint64_t value) {
    for (unsigned i = 8; i; --i)
        out.push_back(static_cast<char>(value >> ((i - 1) * 8)));
}
inline std::string prefix(const std::string& owner) {
    std::string out;
    number(out, owner.size()); out += owner;
    return out;
}
inline std::string page(const std::string& owner, uint64_t id) {
    auto out = prefix(owner); number(out, id);
    return out;
}
inline std::string end(std::string prefix) {
    while (!prefix.empty() && static_cast<unsigned char>(prefix.back()) == 255)
        prefix.pop_back();
    if (prefix.empty())
        throw std::length_error("RocksDB owner prefix overflow");
    prefix.back() = static_cast<char>(static_cast<unsigned char>(prefix.back())
        + 1);
    return prefix;
}
inline unsigned bucket(const std::string& key) {
    return static_cast<unsigned>(acl_hash_crc32(key.data(), key.size()) & 255);
}
inline std::string index(const std::string& key) {
    return std::string(1, static_cast<char>(bucket(key))) + key;
}
}


} // namespace pkv
