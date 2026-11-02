#include "move_journal.h"
#include <cerrno>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace pkv {
int move_journal_io::acquire(const char* path) {
#ifndef _WIN32
    const int fd = ::open(path, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC,
        0600);
    if (fd < 0) return -1;
    struct stat info;
    if (::fstat(fd, &info) || !S_ISREG(info.st_mode) ||
        ::flock(fd, LOCK_EX | LOCK_NB)) {
        const int code = errno ? errno : EINVAL;
        ::close(fd); errno = code; return -1;
    }
    return fd;
#else
    (void) path; errno = ENOTSUP; return -1;
#endif
}
int move_journal_io::existing(const char* path) {
#ifndef _WIN32
    return ::open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
#else
    (void) path; errno = ENOTSUP; return -1;
#endif
}
long long move_journal_io::length(int fd) {
#ifndef _WIN32
    struct stat info;
    if (::fstat(fd, &info)) return -1;
    if (!S_ISREG(info.st_mode)) { errno = EINVAL; return -1; }
    return info.st_size;
#else
    (void) fd; errno = ENOTSUP; return -1;
#endif
}
long long move_journal_io::read(int fd, char* bytes, size_t size) {
#ifndef _WIN32
    return ::read(fd, bytes, size);
#else
    (void) fd; (void) bytes; (void) size; errno = ENOTSUP; return -1;
#endif
}
} // namespace pkv
