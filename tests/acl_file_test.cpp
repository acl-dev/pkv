#include "common/acl_file.h"
#include "common/acl_policy.h"
#include <cerrno>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <fcntl.h>
#include <iostream>
#include <new>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

static thread_local long budget = -1;
void* operator new(std::size_t size) {
    if (!budget) throw std::bad_alloc();
    if (budget > 0) --budget;
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
using namespace pkv;
static void require(bool ok, const char* reason) {
    if (!ok) { std::cerr << reason << '\n'; std::exit(1); }
}
static std::string rules(acl_policy& policy, const std::string& name,
    const std::vector<std::string>& values) {
    std::vector<const std::string*> borrowed;
    for (const auto& value : values) borrowed.push_back(&value);
    return policy.set(name, borrowed);
}
static void reloads() {
    acl_policy policy("startup");
    require(rules(policy, "kept", {"on", "nopass", "+get", "~*"}).empty(),
        "kept setup");
    require(rules(policy, "gone", {"on", "nopass"}).empty(), "gone setup");
    const auto kept = policy.find("kept"), gone = policy.find("gone");
    const auto default_user = policy.find("default");
    acl_reload reload;
    const std::string text = "user default off resetpass -@all\n"
        "user kept off +set ~only:*\nuser added on nopass +get ~*\n";
    require(policy.prepare(text, reload).empty() &&
        reload.removed().size() == 1 && reload.removed()[0] == gone &&
        policy.find("added") == nullptr, "prepare changed live policy");
    budget = 0;
    const auto error = policy.commit(reload);
    budget = -1;
    require(error.empty() && policy.find("kept") == kept &&
        policy.find("default") == default_user && gone->revoked() &&
        policy.find("added") && !policy.default_login(), "reload identity");
    require(!policy.commit(reload).empty(), "reload plan applied twice");
    const auto before = policy.describe();
    for (const auto& invalid : {"# comment\n", "not-user x\n",
        "user same on\nuser same off\n", "user x on +unknown\n",
        "user x on (+get\n", "user x on #invalid\n"}) {
        require(!policy.prepare(invalid, reload).empty() &&
            policy.describe() == before, "bad file changed policy");
    }
    require(!policy.prepare(std::string("user x\0 on", 11), reload).empty(),
        "file NUL accepted");
    require(policy.prepare("", reload).empty() && policy.commit(reload).empty()
        && policy.default_login() && kept->revoked(), "empty-file default");
    require(policy.find("default") == default_user,
        "default identity changed on fallback");
    require(policy.prepare("user bob on nopass +get ~*", reload).empty(),
        "stale plan setup");
    require(rules(policy, "other", {"on"}).empty() &&
        !policy.commit(reload).empty() && !policy.find("bob"), "stale commit");
    acl_policy different;
    require(!different.commit(reload).empty(), "foreign plan committed");
}
static void roundtrip_and_intent() {
    acl_policy policy;
    require(rules(policy, "quoted\"name", {"on", "nopass", "+@all",
        "-@admin", "+client|id", "%R~*", "&a\\b",
        "(+@string ~own:*)"}).empty(), "intent setup");
    const auto text = policy.describe();
    require(text.find("-@admin") != std::string::npos &&
        text.find("+@string") != std::string::npos,
        "category intent flattened");
    acl_policy restored;
    acl_reload reload;
    require(restored.prepare(text, reload).empty() &&
        restored.commit(reload).empty() && restored.describe() == text,
        "quoted/category file replay");
    const auto before = policy.find("quoted\"name")->snapshot();
    require(rules(policy, "quoted\"name", {"+@read", "-get", "+@read"}).empty(),
        "repeated category");
    const auto after = policy.find("quoted\"name")->snapshot();
    require(after->selectors[0].commands.count("get") &&
        before->selectors[0].commands.count("acl|setuser") == 0,
        "history did not follow compiled permissions");
}
static void allocation_failures() {
    acl_policy policy;
    require(rules(policy, "old", {"on", "nopass", "+get", "~*"}).empty(),
        "allocation setup");
    const auto before = policy.describe();
    const auto identity = policy.find("old");
    const std::string text("user new on nopass +@all ~* &*\n");
    size_t failed = 0, passed = 0;
    for (long count = 0; count < 512; ++count) {
        acl_reload reload;
        bool oom = false;
        budget = count;
        try { require(policy.prepare(text, reload).empty(), "valid load"); }
        catch (const std::bad_alloc&) { oom = true; }
        budget = -1;
        require(policy.describe() == before && !identity->revoked() &&
            !policy.find("new"), "OOM prepare published state");
        if (oom) ++failed; else ++passed;
    }
    require(failed && passed, "load allocation coverage");
}
static void concurrent_reloads() {
    acl_policy policy;
    require(rules(policy, "kept", {"on", "nopass", "+get", "~one:*"}).empty(),
        "concurrent load setup");
    const auto kept = policy.find("kept");
    std::atomic<bool> done{false};
    std::vector<std::thread> readers;
    for (unsigned i = 0; i < 4; ++i) readers.emplace_back([&] {
        while (!done.load()) {
            require(policy.find("kept") == kept && !kept->revoked(),
                "reload replaced retained identity");
            const auto settings = kept->snapshot();
            const auto& root = settings->selectors.front();
            require(root.keys.size() == 1 && root.commands.size() == 1 &&
                ((root.keys[0].text == "one:*" && root.commands.count("get")) ||
                 (root.keys[0].text == "two:*" && root.commands.count("set"))),
                "reload published torn settings");
        }
    });
    for (unsigned i = 0; i < 2000; ++i) {
        acl_reload reload;
        const auto text = i % 2 ? "user kept on nopass +get ~one:*\n" :
            "user kept off +set ~two:*\n";
        require(policy.prepare(text, reload).empty() &&
            policy.commit(reload).empty(), "concurrent reload writer");
    }
    done = true;
    for (auto& reader : readers) reader.join();
}
class fault_io : public acl_file_io {
public:
    enum fault { none, parent_open, temp_open, write_error, zero, over_report,
        file_sync, file_close, rename_error, parent_sync } mode = none;
    int file = -1, syncs = 0, writes = 0;
    size_t maximum = 0;
    bool partial = false, interrupted = false;
    int directory(const char* path) override {
        if (mode == parent_open) { errno = EACCES; return -1; }
        const auto fd = acl_file_io::directory(path);
        require(fd < 0 || (::fcntl(fd, F_GETFD) & FD_CLOEXEC),
            "directory descriptor inherited across exec");
        return fd;
    }
    int temporary(char* path) override {
        if (mode == temp_open) { errno = EACCES; return -1; }
        file = acl_file_io::temporary(path);
        require(file < 0 || (::fcntl(file, F_GETFD) & FD_CLOEXEC),
            "temporary descriptor inherited across exec");
        return file;
    }
    long long write(int fd, const char* data, size_t size) override {
        ++writes; maximum = std::max(maximum, size);
        if (interrupted && writes == 1) { errno = EINTR; return -1; }
        if (mode == write_error) { errno = ENOSPC; return -1; }
        if (mode == zero) return 0;
        if (mode == over_report) return static_cast<long long>(size) + 1;
        return acl_file_io::write(fd, data, partial ? std::min(size, size_t(3))
            : size);
    }
    int sync(int fd) override {
        ++syncs;
        if ((mode == file_sync && syncs == 1) ||
            (mode == parent_sync && syncs == 2)) { errno = EIO; return -1; }
        return acl_file_io::sync(fd);
    }
    int close(int fd) override {
        const auto result = acl_file_io::close(fd);
        if (mode == file_close && fd == file) { errno = EIO; return -1; }
        return result;
    }
    int replace(const char* from, const char* to) override {
        if (mode == rename_error) { errno = EIO; return -1; }
        return acl_file_io::replace(from, to);
    }
};
static void write(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text; require(bool(out), "test write");
}
static std::string read(const std::string& path) {
    std::string text;
    require(acl_read_file(path, text).empty(), "test read"); return text;
}
static void disk() {
    char name[] = "/tmp/pkv-acl-file-XXXXXX";
    require(::mkdtemp(name) != nullptr, "test directory");
    const std::string root(name), path = root + "/users.acl";
    const auto marker = path + ".tmp-XXXXXX";
    write(marker, "unowned");
    for (auto fault : {fault_io::parent_open, fault_io::temp_open,
        fault_io::write_error, fault_io::zero, fault_io::over_report,
        fault_io::file_sync, fault_io::file_close, fault_io::rename_error,
        fault_io::parent_sync}) {
        write(path, "old\n");
        fault_io io; io.mode = fault;
        const auto result = acl_save_file(path, "new\n", &io);
        require(!result.ok() && result.replaced ==
            (fault == fault_io::parent_sync), "save failure stage");
        require(read(path) == (result.replaced ? "new\n" : "old\n") &&
            read(marker) == "unowned", "failed save destroyed original file");
    }
    fault_io io; io.partial = true; io.interrupted = true;
    const std::string text(16384, 'a');
    require(acl_save_file(path, text, &io).ok() && read(path) == text &&
        io.writes > 3, "partial/EINTR writes");
    fault_io chunked;
    const std::string large(2 * 1024 * 1024 + 10, 'b');
    require(acl_save_file(path, large, &chunked).ok() && read(path) == large &&
        chunked.maximum <= 1024 * 1024 && chunked.writes == 3, "write chunks");
    struct stat info{};
    require(!::stat(path.c_str(), &info) && (info.st_mode & 0777) == 0600,
        "saved ACL permissions");
    std::string preserved("preserved");
    require(!acl_read_file(root + "/missing", preserved).empty() &&
        preserved == "preserved", "failed read published partial data");
    require(!acl_save_file("", text).ok() &&
        !acl_save_file(std::string("x\0y", 3), text).ok(), "invalid path");
    DIR* directory = ::opendir(root.c_str());
    require(directory != nullptr, "directory listing");
    size_t count = 0;
    while (auto* entry = ::readdir(directory)) {
        if (entry->d_name[0] == '.') continue;
        ++count;
        require(::unlink((root + '/' + entry->d_name).c_str()) == 0, "cleanup");
    }
    ::closedir(directory);
    require(count == 2 && !::rmdir(root.c_str()), "temporary file leak");
}
int main() {
    reloads(); roundtrip_and_intent(); allocation_failures();
    concurrent_reloads(); disk();
    std::cout << "ACL reload staging, identities, intent, OOM and atomic file "
        "IO with concurrent reloads passed\n";
}
