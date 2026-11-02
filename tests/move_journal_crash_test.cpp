#include "move_journal_test_support.h"

struct cutpoint_io : move_journal_io {
    std::string point;
    int directory_fd = -1, temporary_fd = -1;
    unsigned directory_syncs = 0;
    bool written = false;
    void stop(const char* at) {
        if (point != at) return;
        std::cout << "READY " << at << std::endl;
        for (;;) ::pause();
    }
    int directory(const char* path) override {
        return directory_fd = move_journal_io::directory(path);
    }
    int temporary(char* path) override {
        temporary_fd = move_journal_io::temporary(path);
        if (temporary_fd >= 0) stop("temporary");
        return temporary_fd;
    }
    long long write(int fd, const char* bytes, size_t size) override {
        const auto count = move_journal_io::write(fd, bytes, size);
        if (count > 0 && !written) {
            written = true; stop("write");
        }
        return count;
    }
    int sync(int fd) override {
        const auto result = move_journal_io::sync(fd);
        if (!result) {
            if (fd == directory_fd) {
                ++directory_syncs;
                if (directory_syncs == 2) stop("decision-sync");
                if (directory_syncs == 3) stop("completion-sync");
            } else if (fd == temporary_fd) stop("file-sync");
        }
        return result;
    }
    int close(int fd) override {
        const auto result = move_journal_io::close(fd);
        if (fd == temporary_fd) {
            temporary_fd = -1;
            if (!result) stop("close");
        }
        return result;
    }
    int replace(const char* from, const char* to) override {
        const auto result = move_journal_io::replace(from, to);
        if (!result) stop("rename");
        return result;
    }
    int remove(const char* path) override {
        const auto result = move_journal_io::remove(path);
        if (!result) stop("unlink");
        return result;
    }
};
int main(int argc, char** argv) {
    require(argc == 4, "journal helper mode path argument");
    const std::string mode(argv[1]);
    if (mode == "--writer") {
        cutpoint_io io; io.point = argv[3]; move_journal journal(&io);
        require(journal.open(argv[2], journal_identity()) ==
            move_journal::status::empty, "crash helper open");
        require(journal.decide(journal_value()), "crash helper decide");
        io.stop("acknowledged");
        require(journal.complete(journal_value().serial),
            "crash helper complete");
        io.stop("completed");
        require(false, "cutpoint not reached");
    }
    require(mode == "--check", "unknown journal helper mode");
    move_journal journal;
    const bool pending = std::string(argv[3]) == "pending";
    const auto expected = pending ? move_journal::status::pending :
        move_journal::status::empty;
    require(journal.open(argv[2], journal_identity()) == expected,
        "crash journal recovered wrong decision state");
    if (pending) require(same(journal.pending(), journal_value()) &&
        journal.complete(journal_value().serial), "crash journal replay data");
    std::cout << "Journal cutpoint recovery passed\n";
}
