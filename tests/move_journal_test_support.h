#pragma once
#include "db/move_journal.h"
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <fcntl.h>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace pkv;
inline void require(bool ok, const char* reason) {
    if (!ok) { std::cerr << reason << '\n'; std::exit(1); }
}
struct journal_directory {
    std::string path;
    journal_directory() {
        char pattern[] = "/tmp/pkv-move-journal-XXXXXX";
        const auto* created = ::mkdtemp(pattern);
        require(created, "journal temporary directory"); path = created;
    }
    ~journal_directory() {
        DIR* directory = ::opendir(path.c_str());
        if (directory) {
            while (const auto* entry = ::readdir(directory)) {
                const std::string name(entry->d_name);
                if (name != "." && name != "..")
                    ::unlink((path + "/" + name).c_str());
            }
            ::closedir(directory);
        }
        ::rmdir(path.c_str());
    }
    std::string file() const { return path + "/decision"; }
};
inline const std::string& journal_identity() {
    static const char bytes[] = "rdb\0/a;/b;logical=16";
    static const std::string value(bytes, sizeof(bytes) - 1);
    return value;
}
inline move_decision journal_value() {
    move_decision value;
    value.source = 2; value.target = 15; value.serial = 918273;
    value.owner = std::string("key\0\xff\r\n", 7); return value;
}
inline bool same(const move_decision* actual, const move_decision& expected) {
    return actual && actual->source == expected.source &&
        actual->target == expected.target && actual->serial == expected.serial
        && actual->owner == expected.owner;
}
inline std::string file_bytes(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>());
}
inline void replace_bytes(const std::string& path, const std::string& value) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(value.data(), static_cast<std::streamsize>(value.size()));
    require(file.good(), "journal test file write");
}
struct journal_faults : move_journal_io {
    enum fault { none, directory_error, lock_error, existing_error,
        length_error, read_error, read_zero, read_over, temporary_error,
        write_error, write_zero, write_over, file_sync, parent_sync,
        file_close, read_close, rename_error, remove_error } mode = none;
    int parent = -1, temporary_fd = -1, existing_fd = -1;
    bool partial = false, interrupt_read = false, interrupt_write = false;
    bool interrupt_sync = false;
    size_t maximum_write = 0;
    int directory(const char* path) override {
        if (mode == directory_error) { errno = EACCES; return -1; }
        parent = move_journal_io::directory(path);
        require(parent < 0 || (::fcntl(parent, F_GETFD) & FD_CLOEXEC),
            "journal directory inherited across exec");
        return parent;
    }
    int acquire(const char* path) override {
        if (mode == lock_error) { errno = EWOULDBLOCK; return -1; }
        const auto fd = move_journal_io::acquire(path);
        require(fd < 0 || (::fcntl(fd, F_GETFD) & FD_CLOEXEC),
            "journal lock inherited across exec");
        return fd;
    }
    int existing(const char* path) override {
        if (mode == existing_error) { errno = EIO; return -1; }
        existing_fd = move_journal_io::existing(path);
        require(existing_fd < 0 ||
            (::fcntl(existing_fd, F_GETFD) & FD_CLOEXEC),
            "journal record inherited across exec");
        return existing_fd;
    }
    long long length(int fd) override {
        return mode == length_error ? -1 : move_journal_io::length(fd);
    }
    long long read(int fd, char* data, size_t size) override {
        if (interrupt_read) {
            interrupt_read = false; errno = EINTR; return -1;
        }
        if (mode == read_error) { errno = EIO; return -1; }
        if (mode == read_zero) return 0;
        if (mode == read_over) return static_cast<long long>(size) + 1;
        return move_journal_io::read(fd, data,
            partial ? std::min<size_t>(size, 3) : size);
    }
    int temporary(char* path) override {
        if (mode == temporary_error) { errno = ENOSPC; return -1; }
        temporary_fd = move_journal_io::temporary(path);
        require(temporary_fd < 0 ||
            (::fcntl(temporary_fd, F_GETFD) & FD_CLOEXEC),
            "journal temporary inherited across exec");
        return temporary_fd;
    }
    long long write(int fd, const char* data, size_t size) override {
        maximum_write = std::max(maximum_write, size);
        if (interrupt_write) {
            interrupt_write = false; errno = EINTR; return -1;
        }
        if (mode == write_error) { errno = ENOSPC; return -1; }
        if (mode == write_zero) return 0;
        if (mode == write_over) return static_cast<long long>(size) + 1;
        return move_journal_io::write(fd, data,
            partial ? std::min<size_t>(size, 3) : size);
    }
    int sync(int fd) override {
        if (interrupt_sync) {
            interrupt_sync = false; errno = EINTR; return -1;
        }
        if ((mode == parent_sync && fd == parent) ||
            (mode == file_sync && fd != parent)) { errno = EIO; return -1; }
        return move_journal_io::sync(fd);
    }
    int close(int fd) override {
        const int result = move_journal_io::close(fd);
        if (mode == file_close && fd == temporary_fd) {
            temporary_fd = -1; errno = EIO; return -1;
        }
        if (mode == read_close && fd == existing_fd) {
            existing_fd = -1; errno = EIO; return -1;
        }
        return result;
    }
    int replace(const char* from, const char* to) override {
        if (mode == rename_error) { errno = EIO; return -1; }
        return move_journal_io::replace(from, to);
    }
    int remove(const char* path) override {
        if (mode == remove_error) { errno = EIO; return -1; }
        return move_journal_io::remove(path);
    }
};
