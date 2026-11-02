#pragma once
#include "move_domain.h"

namespace pkv {
struct move_domain::implementation {
    move_journal_io native;
    move_journal_io& io;
    std::vector<int> locks;
    std::vector<std::string> roots;
    std::string path, identity, marker;
    std::unique_ptr<move_journal> journal;
    bool attempted = false, prepared = false, bound = false, failed = false;
    explicit implementation(move_journal_io* operations)
    : io(operations ? *operations : native) {}
    ~implementation();
    void prepare(const std::string& backend, const std::string& paths,
        size_t databases, const std::string& journal_path);
    void bind();
    bool read_marker(const std::string& root, bool compare, bool seal = false);
    void validate();
    void lock_roots();
    void encode(const std::string& backend, size_t databases);
    static std::string canonical(const std::string& path);
    static std::string canonical_journal(const std::string& path);
};
} // namespace pkv
