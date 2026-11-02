#include "move_journal_test_support.h"
#include <fcntl.h>
#include <new>

static thread_local long allocation_budget = -1;
void* operator new(std::size_t size) {
    if (!allocation_budget) throw std::bad_alloc();
    if (allocation_budget > 0) --allocation_budget;
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
#if __cplusplus >= 201402L
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
#endif
using state = move_journal::status;
static void roundtrips() {
    journal_directory dir;
    for (const auto& key : {std::string(), journal_value().owner,
        std::string(200000, 'k')}) {
        auto value = journal_value(); value.owner = key;
        {
            journal_faults io;
            io.partial = true; io.interrupt_write = io.interrupt_sync = true;
            move_journal journal(&io);
            require(journal.open(dir.file(), journal_identity()) ==
                state::empty,
                "empty journal open");
            require(journal.decide(value) && same(journal.pending(), value),
                "binary journal decision");
            require(io.maximum_write <= 65536, "unbounded journal write");
            require(!journal.decide(value) && !journal.complete(value.serial+1)
                && same(journal.pending(), value), "pending decision replaced");
            move_journal competitor;
            require(competitor.open(dir.file(), journal_identity()) ==
                state::failed, "journal lock shared");
            struct stat info;
            require(!::stat(dir.file().c_str(), &info) &&
                (info.st_mode & 0777) == 0600, "journal file permissions");
        }
        {
            journal_faults io; io.partial = io.interrupt_read = true;
            move_journal journal(&io);
            require(journal.open(dir.file(), journal_identity()) ==
                state::pending && same(journal.pending(), value),
                "journal independent recovery");
            require(journal.complete(value.serial) && !journal.pending() &&
                journal.state() == state::empty &&
                !journal.complete(value.serial), "journal completion");
            require(!journal.decide(value) && journal.state() == state::empty,
                "journal reused serial");
            auto next = value; ++next.serial;
            require(journal.decide(next) && !journal.complete(value.serial)
                && same(journal.pending(), next) &&
                journal.complete(next.serial), "stale serial cleared decision");
        }
        move_journal empty;
        require(empty.open(dir.file(), journal_identity()) == state::empty,
            "completed journal resurrected");
    }
}
static void damaged() {
    journal_directory dir;
    {
        move_journal journal;
        require(journal.open(dir.file(), journal_identity()) == state::empty
            && journal.decide(journal_value()), "damage seed");
    }
    const auto original = file_bytes(dir.file());
    auto rejects = [&](const std::string& bytes) {
        replace_bytes(dir.file(), bytes);
        move_journal journal;
        require(journal.open(dir.file(), journal_identity()) == state::failed
            && !journal.pending() && !journal.decide(journal_value()) &&
            file_bytes(dir.file()) == bytes, "damaged decision accepted/lost");
    };
    for (size_t i = 0; i < original.size(); ++i) {
        rejects(original.substr(0, i));
        auto bytes = original; bytes[i] ^= 0x40; rejects(bytes);
    }
    rejects(original + "extra");
    replace_bytes(dir.file(), original);
    {
        move_journal wrong;
        require(wrong.open(dir.file(), "different configuration") ==
            state::failed, "changed storage identity accepted");
    }
    ::unlink(dir.file().c_str());
    require(!::symlink((dir.path + "/absent").c_str(), dir.file().c_str()),
        "symlink seed");
    move_journal link;
    require(link.open(dir.file(), journal_identity()) == state::failed,
        "journal followed symlink");
}
static void save_faults() {
    for (const auto fault : {journal_faults::temporary_error,
        journal_faults::write_error, journal_faults::write_zero,
        journal_faults::write_over, journal_faults::file_sync,
        journal_faults::file_close, journal_faults::rename_error,
        journal_faults::parent_sync}) {
        journal_directory dir;
        {
            journal_faults io; move_journal journal(&io);
            require(journal.open(dir.file(), journal_identity()) ==
                state::empty,
                "fault empty open");
            io.mode = fault;
            require(!journal.decide(journal_value()) &&
                journal.state() == state::failed && !journal.pending() &&
                !journal.decide(journal_value()), "write failure not sticky");
        }
        move_journal recovery;
        const auto expected = fault == journal_faults::parent_sync ?
            state::pending : state::empty;
        require(recovery.open(dir.file(), journal_identity()) == expected,
            "uncertain rename discarded or predecision became committed");
    }
}
static void completion_faults() {
    for (const auto fault : {journal_faults::remove_error,
        journal_faults::parent_sync}) {
        journal_directory dir;
        {
            journal_faults io; move_journal journal(&io);
            require(journal.open(dir.file(), journal_identity()) == state::empty
                && journal.decide(journal_value()), "completion fault seed");
            io.mode = fault;
            require(!journal.complete(journal_value().serial) &&
                journal.state() == state::failed &&
                !journal.decide(journal_value()), "completion failure reused");
        }
        move_journal recovery;
        require(recovery.open(dir.file(), journal_identity()) ==
            (fault == journal_faults::remove_error ? state::pending :
                state::empty), "completion fault recovery");
    }
}
static void read_faults() {
    journal_directory dir;
    {
        move_journal seed;
        require(seed.open(dir.file(), journal_identity()) == state::empty &&
            seed.decide(journal_value()), "read fault seed");
    }
    for (const auto fault : {journal_faults::directory_error,
        journal_faults::lock_error, journal_faults::existing_error,
        journal_faults::length_error, journal_faults::read_error,
        journal_faults::read_zero, journal_faults::read_over,
        journal_faults::file_sync, journal_faults::parent_sync,
        journal_faults::read_close}) {
        journal_faults io; io.mode = fault; move_journal journal(&io);
        require(journal.open(dir.file(), journal_identity()) == state::failed
            && !journal.pending(), "read fault admitted recovery");
    }
}
static void allocation_faults() {
    size_t failed = 0, succeeded = 0;
    for (long count = 0; count < 48; ++count) {
        journal_directory dir;
        const auto path = dir.file();
        auto value = journal_value(); value.owner.assign(100000, 'v');
        bool saved = false;
        {
            move_journal journal;
            require(journal.open(path, journal_identity()) == state::empty,
                "allocation seed");
            allocation_budget = count;
            saved = journal.decide(value);
            allocation_budget = -1;
            if (saved) ++succeeded; else ++failed;
            require(saved || journal.state() == state::failed,
                "allocation failure not sticky");
        }
        move_journal reopened;
        require(reopened.open(path, journal_identity()) ==
            (saved ? state::pending : state::empty),
            "allocation recovery failed");
    }
    require(failed && succeeded, "allocation budgets missed boundary");
    std::cout << "Journal allocation budgets: failed=" << failed <<
        " succeeded=" << succeeded << '\n';
}
static void boundaries() {
    journal_directory dir;
    for (const auto& path : {std::string(), std::string("relative"),
        dir.path + "/", dir.file() + std::string("\0bad", 4)}) {
        move_journal invalid;
        require(invalid.open(path, journal_identity()) == state::failed,
            "invalid journal path accepted");
    }
    for (const auto& identity : {std::string(), std::string(65537, 'x')}) {
        move_journal invalid;
        require(invalid.open(dir.file(), identity) == state::failed,
            "invalid journal identity accepted");
    }
    {
        move_journal unopened;
        require(!unopened.decide(journal_value()) &&
            !unopened.complete(1), "unopened journal accepted work");
        require(unopened.open(dir.file(), journal_identity()) == state::empty,
            "invalid calls changed unopened journal");
        auto value = journal_value(); value.target = value.source;
        require(!unopened.decide(value) && unopened.state() == state::failed,
            "same database decision accepted");
    }
    ::unlink((dir.file() + ".lock").c_str());
    require(!::symlink((dir.path + "/absent").c_str(),
        (dir.file() + ".lock").c_str()), "lock symlink seed");
    {
        move_journal invalid;
        require(invalid.open(dir.file(), journal_identity()) == state::failed,
            "lock symlink followed");
    }
    ::unlink((dir.file() + ".lock").c_str());
    {
        move_journal valid;
        require(valid.open(dir.file(), journal_identity()) == state::empty &&
            valid.decide(journal_value()), "sparse journal seed");
    }
    const auto original = file_bytes(dir.file());
    auto oversized = original;
    for (size_t j = 28; j < 32; ++j) oversized[j] = '\xff';
    replace_bytes(dir.file(), oversized);
    move_journal invalid;
    require(invalid.open(dir.file(), journal_identity()) == state::failed,
        "oversized owner admitted");
}
static void open_allocations() {
    journal_directory dir;
    const auto path = dir.file();
    auto value = journal_value(); value.owner.assign(100000, 'r');
    {
        move_journal seed;
        require(seed.open(path, journal_identity()) == state::empty &&
            seed.decide(value), "open allocation seed");
    }
    const auto original = file_bytes(path);
    size_t failed = 0, succeeded = 0;
    for (long count = 0; count < 48; ++count) {
        move_journal loaded;
        allocation_budget = count;
        const auto status = loaded.open(path, journal_identity());
        allocation_budget = -1;
        if (status == state::pending) {
            ++succeeded;
            require(same(loaded.pending(), value), "OOM load corrupted owner");
        } else {
            ++failed;
            require(status == state::failed && !loaded.pending(),
                "OOM open exposed partial decision");
        }
        require(file_bytes(path) == original, "OOM open modified record");
    }
    require(failed && succeeded, "open allocation coverage");
    std::cout << "Journal load budgets: failed=" << failed <<
        " succeeded=" << succeeded << '\n';
}
struct ordering_io : move_journal_io {
    int parent = -1, file = -1;
    std::vector<std::string> events;
    int directory(const char* path) override {
        return parent = move_journal_io::directory(path);
    }
    int temporary(char* path) override {
        return file = move_journal_io::temporary(path);
    }
    long long write(int fd, const char* bytes, size_t size) override {
        events.push_back("write");
        return move_journal_io::write(fd, bytes, size);
    }
    int sync(int fd) override {
        events.push_back(fd == parent ? "parent-sync" : "file-sync");
        return move_journal_io::sync(fd);
    }
    int close(int fd) override {
        if (fd == file) { file = -1; events.push_back("file-close"); }
        return move_journal_io::close(fd);
    }
    int replace(const char* from, const char* to) override {
        events.push_back("rename");
        return move_journal_io::replace(from, to);
    }
    int remove(const char* path) override {
        events.push_back("unlink"); return move_journal_io::remove(path);
    }
};
static void persistence_order() {
    journal_directory dir;
    ordering_io io; move_journal journal(&io);
    require(journal.open(dir.file(), journal_identity()) == state::empty,
        "order empty open");
    require(io.events == std::vector<std::string>{"parent-sync"},
        "empty journal directory not synchronized");
    io.events.clear();
    require(journal.decide(journal_value()) && io.events ==
        std::vector<std::string>{"write", "write", "write", "write",
            "file-sync", "file-close", "rename", "parent-sync"},
        "decision persistence order changed");
    io.events.clear();
    require(journal.complete(journal_value().serial) && io.events ==
        std::vector<std::string>{"unlink", "parent-sync"},
        "completion persistence order changed");
}
int main() {
    roundtrips(); damaged(); save_faults(); completion_faults(); read_faults();
    allocation_faults(); boundaries(); open_allocations(); persistence_order();
    std::cout << "MOVE journal checksums, locks, faults and recovery passed\n";
}
