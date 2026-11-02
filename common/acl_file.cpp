#include "acl_file.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <vector>
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#endif

namespace pkv {
int acl_file_io::temporary(char* path) {
#ifndef _WIN32
    const auto fd = ::mkstemp(path);
    if (fd >= 0 && ::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
        const auto code = errno;
        ::close(fd); ::unlink(path); errno = code; return -1;
    }
    return fd;
#else
    (void) path; errno = ENOTSUP; return -1;
#endif
}
int acl_file_io::directory(const char* path) {
#ifndef _WIN32
    return ::open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
#else
    (void) path; errno = ENOTSUP; return -1;
#endif
}
long long acl_file_io::write(int fd, const char* data, size_t size) {
#ifndef _WIN32
    return ::write(fd, data, size);
#else
    (void) fd; (void) data; (void) size; errno = ENOTSUP; return -1;
#endif
}
int acl_file_io::sync(int fd) {
#ifndef _WIN32
    return ::fsync(fd);
#else
    (void) fd; errno = ENOTSUP; return -1;
#endif
}
int acl_file_io::close(int fd) {
#ifndef _WIN32
    return ::close(fd);
#else
    (void) fd; errno = ENOTSUP; return -1;
#endif
}
int acl_file_io::replace(const char* from, const char* to) {
    return std::rename(from, to);
}
int acl_file_io::remove(const char* path) { return std::remove(path); }
namespace {
bool valid_path(const std::string& path) {
    return !path.empty() && path.find('\0') == std::string::npos;
}
std::string failure(const char* operation, int code) {
    return "ERR ACL file " + std::string(operation) + ": " +
        std::strerror(code ? code : EIO);
}
class pending_file {
public:
    acl_file_io& io;
    int file = -1, parent = -1;
    std::vector<char> name;
    bool created = false, renamed = false;
    pending_file(acl_file_io& operations, const std::string& path)
    : io(operations), name(path.begin(), path.end()) { name.push_back('\0'); }
    ~pending_file() {
        if (file >= 0) io.close(file);
        if (parent >= 0) io.close(parent);
        if (created && !renamed) io.remove(name.data());
    }
};
} // namespace
std::string acl_read_file(const std::string& path, std::string& text) {
    if (!valid_path(path)) return failure("path", EINVAL);
    std::ifstream input(path, std::ios::binary);
    if (!input) return failure("open", errno);
    std::string staged;
    char buffer[8192];
    while (input.read(buffer, sizeof(buffer)) || input.gcount()) {
        staged.append(buffer, static_cast<size_t>(input.gcount()));
        if (input.bad()) return failure("read", errno);
    }
    if (!input.eof()) return failure("read", errno);
    text.swap(staged);
    return {};
}
acl_file_result acl_save_file(const std::string& path, const std::string& text,
    acl_file_io* operations) {
    acl_file_result result;
    if (!valid_path(path)) {
        result.error = failure("path", EINVAL); return result;
    }
    acl_file_io system;
    auto& io = operations ? *operations : system;
    pending_file pending(io, path + ".tmp-XXXXXX");
    const auto slash = path.find_last_of('/');
    const auto parent = slash == std::string::npos ? "." :
        slash == 0 ? "/" : path.substr(0, slash);
    pending.parent = io.directory(parent.c_str());
    if (pending.parent < 0) {
        result.error = failure("directory open", errno); return result;
    }
    pending.file = io.temporary(pending.name.data());
    if (pending.file < 0) {
        result.error = failure("temporary open", errno); return result;
    }
    pending.created = true;
    size_t offset = 0;
    while (offset < text.size()) {
        const auto count = std::min<size_t>(text.size() - offset, 1024 * 1024);
        const auto written = io.write(pending.file,
            text.data() + offset, count);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0 || static_cast<unsigned long long>(written) > count) {
            result.error = failure("write", written < 0 ? errno : EIO);
            return result;
        }
        offset += static_cast<size_t>(written);
    }
    if (io.sync(pending.file)) {
        result.error = failure("sync", errno); return result;
    }
    const auto closed = io.close(pending.file); pending.file = -1;
    if (closed) {
        result.error = failure("close", errno); return result;
    }
    if (io.replace(pending.name.data(), path.c_str())) {
        result.error = failure("rename", errno); return result;
    }
    pending.renamed = true; result.replaced = true;
    if (io.sync(pending.parent))
        result.error = failure("directory sync", errno);
    return result;
}
} // namespace pkv
