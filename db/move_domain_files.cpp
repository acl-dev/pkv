#include "move_domain_internal.h"
#include <cerrno>
#include <algorithm>
#include <cstdlib>
#include <stdexcept>
#ifndef _WIN32
#include <stdlib.h>
#endif

namespace pkv {
std::string move_domain::implementation::canonical(const std::string& path) {
#ifndef _WIN32
    if (path.empty() || path.find('\0') != path.npos)
        throw std::runtime_error("invalid move domain path");
    std::unique_ptr<char, decltype(&std::free)> resolved(
        ::realpath(path.c_str(), nullptr), &std::free);
    if (!resolved) throw std::runtime_error("cannot resolve move domain path");
    return resolved.get();
#else
    (void) path; throw std::runtime_error("move domain requires POSIX storage");
#endif
}
std::string move_domain::implementation::canonical_journal(
    const std::string& input) {
    if (input.empty() || input[0] != '/' || input.back() == '/' ||
        input.find('\0') != input.npos)
        throw std::runtime_error("MOVE journal path must be absolute");
    const auto split = input.find_last_of('/');
    const auto name = input.substr(split + 1);
    if (name == "." || name == "..")
        throw std::runtime_error("invalid MOVE journal basename");
    const auto parent = canonical(split ? input.substr(0, split) : "/");
    return parent == "/" ? parent + name : parent + "/" + name;
}
bool move_domain::implementation::read_marker(const std::string& root,
    bool compare, bool seal) {
    struct descriptor {
        move_journal_io& io;
        int fd;
        ~descriptor() { if (fd >= 0) io.close(fd); }
    } file{io, io.existing((root + "/.pkv-move.domain").c_str())};
    if (file.fd < 0) {
        if (errno == ENOENT) return false;
        throw std::runtime_error("cannot inspect move domain marker");
    }
    if (!compare) return true;
    const auto size = io.length(file.fd);
    if (size < 0 || static_cast<uint64_t>(size) != marker.size())
        throw std::runtime_error("move domain marker size mismatch");
    std::string bytes(marker.size(), '\0');
    size_t offset = 0;
    while (offset < bytes.size()) {
        const auto requested = std::min<size_t>(65536, bytes.size() - offset);
        const auto count = io.read(file.fd, &bytes[offset], requested);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0 || static_cast<unsigned long long>(count) >
            requested)
            throw std::runtime_error("cannot read move domain marker");
        offset += static_cast<size_t>(count);
    }
    if (bytes != marker)
        throw std::runtime_error("move domain configuration mismatch");
    auto sync = [&](int fd) {
        while (io.sync(fd)) if (errno != EINTR)
            throw std::runtime_error("cannot synchronize move domain marker");
    };
    if (seal) sync(file.fd);
    const int closed = io.close(file.fd); file.fd = -1;
    if (closed) throw std::runtime_error("cannot close move domain marker");
    if (seal) {
        descriptor parent{io, io.directory(root.c_str())};
        if (parent.fd < 0)
            throw std::runtime_error("cannot open move domain directory");
        sync(parent.fd);
    }
    return true;
}
} // namespace pkv
