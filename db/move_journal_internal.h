#pragma once
#include "move_journal.h"
#include <array>

namespace pkv {
struct move_journal::implementation {
    struct descriptor {
        move_journal_io& io;
        int fd = -1;
        explicit descriptor(move_journal_io& ops) : io(ops) {}
        ~descriptor() { if (fd >= 0) io.close(fd); }
        bool close() {
            const int value = fd; fd = -1;
            return io.close(value) == 0;
        }
    };
    move_journal_io native;
    move_journal_io& io;
    descriptor lock, directory;
    status current = status::unopened;
    std::string path, identity;
    move_decision decision;
    explicit implementation(move_journal_io* operations)
    : io(operations ? *operations : native), lock(io), directory(io) {}
    bool sync(int fd);
    bool write(int fd, const char* bytes, size_t size);
    bool read(int fd, char* bytes, size_t size);
    bool load();
    bool save(const move_decision& value);
    static std::array<char, 32> header(const move_decision& value,
        size_t identity_size);
};
} // namespace pkv
