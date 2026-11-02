#pragma once
#include "common/acl_file.h"
#include <cstdint>
#include <memory>
#include <string>

namespace pkv {
struct move_decision {
    uint32_t source = 0, target = 0;
    uint64_t serial = 0;
    std::string owner;
};
class move_journal_io : public acl_file_io {
public:
    virtual int acquire(const char* path);
    virtual int existing(const char* path);
    virtual long long length(int fd);
    virtual long long read(int fd, char* bytes, size_t size);
};
// One outstanding redo decision, serialized by its coordinator. This class is
// not a DB transaction or a public MOVE implementation. The caller must fence
// the locked source before decide(), retain both DB/key locks, and fence both
// applied engines before complete(). Recovery precedes traffic and expiry.
class move_journal {
public:
    enum class status { unopened, empty, pending, failed };
    explicit move_journal(move_journal_io* operations = nullptr);
    ~move_journal();
    move_journal(const move_journal&) = delete;
    move_journal& operator=(const move_journal&) = delete;
    // Absolute path, existing parent, exact nonempty storage configuration
    // identity (including engine, ordered canonical paths and logical count).
    // A surviving record is validated and synced before returning pending.
    status open(const std::string& path, const std::string& identity);
    status state() const;
    uint64_t last_serial() const;
    // Nonmutating validation before the coordinator attempts publication.
    bool can_decide(const move_decision& decision) const;
    const move_decision* pending() const;
    // Serials strictly increase during this instance's locked lifetime.
    // A failed IO/validation result poisons the instance until destruction.
    bool decide(const move_decision& decision);
    bool complete(uint64_t serial);
private:
    struct implementation;
    std::unique_ptr<implementation> data_;
};
} // namespace pkv
