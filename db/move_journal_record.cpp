#include "move_journal_internal.h"
#include "rpc/page_wire.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <vector>
#include <type_traits>
#include <zlib.h>

namespace pkv {
static_assert(std::is_nothrow_move_assignable<move_decision>::value,
    "Decision publication after rename must not throw");
namespace {
const size_t maximum_key = 512u * 1024 * 1024;
const size_t maximum_identity = 65536;
uint32_t checksum(const std::array<char, 32>& header,
    const std::string& identity, const std::string& owner) {
    auto crc = ::crc32(0, reinterpret_cast<const Bytef*>(header.data()),
        static_cast<uInt>(header.size()));
    for (const auto* part : {&identity, &owner})
        crc = ::crc32(crc, reinterpret_cast<const Bytef*>(part->data()),
            static_cast<uInt>(part->size()));
    return static_cast<uint32_t>(crc);
}
struct temporary_file {
    move_journal_io& io;
    int fd = -1;
    std::vector<char> name;
    bool created = false, renamed = false;
    temporary_file(move_journal_io& ops, const std::string& path)
    : io(ops), name(path.begin(), path.end()) { name.push_back('\0'); }
    ~temporary_file() {
        if (fd >= 0) io.close(fd);
        if (created && !renamed) io.remove(name.data());
    }
};
} // namespace
bool move_journal::implementation::sync(int fd) {
    for (;;) {
        if (!io.sync(fd)) return true;
        if (errno != EINTR) return false;
    }
}
bool move_journal::implementation::write(int fd, const char* bytes,
    size_t size) {
    while (size) {
        const auto amount = std::min<size_t>(size, 65536);
        const auto count = io.write(fd, bytes, amount);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0 || static_cast<unsigned long long>(count) > amount)
            return false;
        bytes += count; size -= static_cast<size_t>(count);
    }
    return true;
}
bool move_journal::implementation::read(int fd, char* bytes, size_t size) {
    while (size) {
        const auto amount = std::min<size_t>(size, 65536);
        const auto count = io.read(fd, bytes, amount);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0 || static_cast<unsigned long long>(count) > amount)
            return false;
        bytes += count; size -= static_cast<size_t>(count);
    }
    return true;
}
std::array<char, 32> move_journal::implementation::header(
    const move_decision& value, size_t identity_size) {
    std::array<char, 32> bytes;
    std::memcpy(bytes.data(), "PKVMOVE1", 8);
    page_wire::put32(bytes.data() + 8, value.source);
    page_wire::put32(bytes.data() + 12, value.target);
    page_wire::put64(bytes.data() + 16, value.serial);
    page_wire::put32(bytes.data() + 24,
        static_cast<uint32_t>(identity_size));
    page_wire::put32(bytes.data() + 28,
        static_cast<uint32_t>(value.owner.size()));
    return bytes;
}
bool move_journal::implementation::load() {
    descriptor file(io);
    file.fd = io.existing(path.c_str());
    if (file.fd < 0) {
        if (errno != ENOENT || !sync(directory.fd)) return false;
        current = status::empty; return true;
    }
    const auto size = io.length(file.fd);
    std::array<char, 32> bytes;
    if (size < 36 || !read(file.fd, bytes.data(), bytes.size()) ||
        std::memcmp(bytes.data(), "PKVMOVE1", 8)) return false;
    const auto domain_size = page_wire::get32(bytes.data() + 24);
    const auto key_size = page_wire::get32(bytes.data() + 28);
    if (!domain_size || domain_size > maximum_identity ||
        key_size > maximum_key || domain_size != identity.size() ||
        static_cast<uint64_t>(size) != 36ull + domain_size + key_size)
        return false;
    move_decision staged;
    staged.source = page_wire::get32(bytes.data() + 8);
    staged.target = page_wire::get32(bytes.data() + 12);
    staged.serial = page_wire::get64(bytes.data() + 16);
    if (!staged.serial || staged.source == staged.target) return false;
    std::string domain(domain_size, '\0');
    if (!read(file.fd, &domain[0], domain.size()) || domain != identity)
        return false;
    staged.owner.resize(key_size);
    if (key_size && !read(file.fd, &staged.owner[0], key_size)) return false;
    char footer[4];
    if (!read(file.fd, footer, sizeof(footer)) ||
        page_wire::get32(footer) != checksum(bytes, domain, staged.owner))
        return false;
    if (!sync(file.fd) || !file.close() || !sync(directory.fd)) return false;
    decision = std::move(staged); current = status::pending;
    return true;
}
bool move_journal::can_decide(const move_decision& value) const {
    return state() == status::empty && value.serial &&
        value.serial > last_serial() && value.source != value.target &&
        value.owner.size() <= maximum_key;
}
bool move_journal::implementation::save(const move_decision& value) {
    if (!value.serial || value.source == value.target ||
        value.owner.size() > maximum_key) return false;
    // All record copies and path allocation precede any durable decision.
    move_decision staged(value);
    const auto bytes = header(value, identity.size());
    char footer[4];
    page_wire::put32(footer, checksum(bytes, identity, value.owner));
    temporary_file file(io, path + ".tmp-XXXXXX");
    file.fd = io.temporary(file.name.data());
    if (file.fd < 0) return false;
    file.created = true;
    if (!write(file.fd, bytes.data(), bytes.size()) ||
        !write(file.fd, identity.data(), identity.size()) ||
        !write(file.fd, value.owner.data(), value.owner.size()) ||
        !write(file.fd, footer, sizeof(footer)) || !sync(file.fd))
        return false;
    const auto result = io.close(file.fd); file.fd = -1;
    if (result || io.replace(file.name.data(), path.c_str())) return false;
    file.renamed = true;
    if (!sync(directory.fd)) return false;
    decision = std::move(staged); current = status::pending;
    return true;
}
} // namespace pkv
