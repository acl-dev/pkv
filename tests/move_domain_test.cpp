#include "stdafx.h"
#include "db/move_domain.h"
#include "move_journal_test_support.h"
#include <sys/stat.h>

static void remove_tree(const std::string& path) {
    auto* directory = ::opendir(path.c_str());
    if (!directory) return;
    while (const auto* item = ::readdir(directory)) {
        const std::string name(item->d_name);
        if (name == "." || name == "..") continue;
        const auto child = path + "/" + name;
        struct stat info;
        if (!::lstat(child.c_str(), &info) && S_ISDIR(info.st_mode))
            remove_tree(child);
        else ::unlink(child.c_str());
    }
    ::closedir(directory); ::rmdir(path.c_str());
}
struct scratch {
    std::string root, paths, log;
    scratch() {
        char name[] = "/tmp/pkv-domain-XXXXXX";
        const auto* value = ::mkdtemp(name); require(value, "domain scratch");
        root = value; paths = root + "/a;" + root + "/b";
        log = root + "/decision";
    }
    ~scratch() { remove_tree(root); }
    std::string marker(char part) const {
        return root + "/" + part + "/.pkv-move.domain";
    }
};
static void lifecycle() {
    scratch files;
    {
        move_domain disabled;
        require(disabled.prepare("rdb", files.paths, 16, "").empty() &&
            !disabled.enabled() && disabled.bind().empty() &&
            !disabled.journal() && ::access((files.root + "/a").c_str(), F_OK),
            "disabled journal created storage state");
    }
    std::string identity;
    {
        move_domain domain;
        require(domain.prepare("rdb", files.paths, 16, files.log).empty() &&
            domain.enabled() && !domain.journal() &&
            ::access(files.marker('a').c_str(), F_OK),
            "prepare published state");
        identity = domain.identity();
        move_domain other;
        require(!other.prepare("rdb", files.paths, 16, files.log).empty(),
            "domain ownership shared");
        require(domain.bind().empty() && domain.journal() &&
            domain.journal()->state() == move_journal::status::empty,
            "domain bind");
        move_decision decision;
        decision.source = 0; decision.target = 1; decision.serial = 9;
        decision.owner = std::string("key\0binary", 10);
        require(domain.journal()->decide(decision), "domain pending seed");
    }
    {
        move_domain restored;
        require(restored.prepare("RDB", files.paths, 16, files.log).empty() &&
            restored.identity() == identity && restored.bind().empty() &&
            restored.journal()->pending() &&
            restored.journal()->pending()->owner ==
                std::string("key\0binary", 10), "domain lost pending decision");
        require(restored.journal()->complete(9), "domain journal clear");
    }
    move_domain disabled;
    require(!disabled.prepare("rdb", files.paths, 16, "").empty(),
        "disabled binding ignored markers");
}
static void configuration() {
    scratch files;
    {
        move_domain domain;
        require(domain.prepare("rdb", files.paths, 16, files.log).empty() &&
            domain.bind().empty(), "configuration seed");
    }
    const auto original = file_bytes(files.marker('a'));
    for (unsigned variant = 0; variant < 5; ++variant) {
        move_domain domain;
        const auto paths = variant == 0 ? files.root + "/b;" +
            files.root + "/a" : variant == 1 ? files.root + "/a;" +
            files.root + "/c" : files.paths;
        require(!domain.prepare(variant == 2 ? "wdb" : "rdb", paths,
            variant == 3 ? 17 : 16, variant == 4 ? files.log + "2" :
            files.log).empty(), "configuration mismatch accepted");
        require(file_bytes(files.marker('a')) == original,
            "mismatched startup replaced marker");
    }
    for (const auto& conflict : {files.marker('a'),
        files.root + "/a/.pkv-move.domain.lock", files.root + "/a/rdb/log"}) {
        move_domain domain;
        require(!domain.prepare("rdb", files.paths, 16, conflict).empty(),
            "journal overlaps storage namespace");
    }
    move_domain memory;
    require(!memory.prepare("mdb", files.paths, 16, files.log).empty(),
        "volatile journal advertised persistence");
    require(!::symlink(files.root.c_str(),
        (files.root + "/alias").c_str()), "journal alias");
    move_domain alias;
    require(alias.prepare("rdb", files.paths, 16,
        files.root + "/alias/decision").empty() && alias.bind().empty(),
        "canonical journal alias rejected");
}
static void damaged_and_partial() {
    scratch files;
    {
        move_domain domain;
        require(domain.prepare("rdb", files.paths, 16, files.log).empty() &&
            domain.bind().empty(), "damage seed");
    }
    const auto original = file_bytes(files.marker('a'));
    for (size_t i = 0; i < original.size(); ++i) {
        auto bytes = original; bytes[i] ^= 0x40;
        replace_bytes(files.marker('a'), bytes);
        move_domain broken;
        require(!broken.prepare("rdb", files.paths, 16, files.log).empty() &&
            file_bytes(files.marker('a')) == bytes,
            "damaged identity accepted or overwritten");
    }
    replace_bytes(files.marker('a'), original);
    ::unlink(files.marker('b').c_str());
    {
        move_domain partial;
        require(partial.prepare("rdb", files.paths, 16, files.log).empty() &&
            ::access(files.marker('b').c_str(), F_OK) && partial.bind().empty()
            && file_bytes(files.marker('b')) == original,
            "partial binding not resumed");
    }
    ::unlink(files.marker('a').c_str());
    require(!::symlink(files.marker('b').c_str(), files.marker('a').c_str()),
        "marker symlink seed");
    move_domain symlink;
    require(!symlink.prepare("rdb", files.paths, 16, files.log).empty(),
        "marker symlink followed");
}
static void io_faults() {
    for (const auto mode : {journal_faults::write_error,
        journal_faults::file_sync, journal_faults::file_close,
        journal_faults::rename_error, journal_faults::parent_sync}) {
        scratch files;
        {
            journal_faults io;
            move_domain domain(&io);
            require(domain.prepare("rdb", files.paths, 16, files.log).empty(),
                "binding fault prepare");
            io.mode = mode;
            require(!domain.bind().empty() && !domain.journal() &&
                !domain.enabled(), "failed binding exposed journal");
        }
        move_domain recovered;
        require(recovered.prepare("rdb", files.paths, 16, files.log).empty()
            && recovered.bind().empty() && recovered.journal(),
            "failed binding cannot resume");
    }
}
static void existing_marker_faults() {
    scratch files;
    {
        move_domain domain;
        require(domain.prepare("rdb", files.paths, 16, files.log).empty() &&
            domain.bind().empty(), "existing marker seed");
    }
    const auto original = file_bytes(files.marker('a'));
    for (const auto mode : {journal_faults::existing_error,
        journal_faults::length_error, journal_faults::read_error,
        journal_faults::read_zero, journal_faults::read_over,
        journal_faults::read_close}) {
        journal_faults io; io.mode = mode;
        move_domain domain(&io);
        require(!domain.prepare("rdb", files.paths, 16, files.log).empty() &&
            !domain.journal() && file_bytes(files.marker('a')) == original,
            "marker read failure ignored or rewrote marker");
    }
    for (const auto mode : {journal_faults::file_sync,
        journal_faults::parent_sync, journal_faults::directory_error,
        journal_faults::read_close}) {
        journal_faults io;
        move_domain domain(&io);
        require(domain.prepare("rdb", files.paths, 16, files.log).empty(),
            "existing seal prepare");
        io.mode = mode;
        require(!domain.bind().empty() && !domain.journal() &&
            file_bytes(files.marker('a')) == original,
            "existing marker seal failure ignored");
    }
    journal_faults io; io.partial = true; io.interrupt_read = true;
    move_domain domain(&io);
    require(domain.prepare("rdb", files.paths, 16, files.log).empty(),
        "partial interrupted marker read");
    io.interrupt_sync = true;
    require(domain.bind().empty() && domain.journal(),
        "interrupted existing marker seal");
}
static void automatic() {
    for (const auto* backend : {"rdb", "wdb"}) {
        scratch files;
        std::string identity, path;
        {
            move_domain domain;
            require(domain.prepare(backend, files.paths, 16, "auto").empty()
                && domain.enabled() && domain.bind().empty() &&
                domain.journal(), "automatic native journal bind");
            path = domain.path(); identity = domain.identity();
            require(path == domain.roots().front() +
                "/.pkv-move.decision", "automatic journal location");
        }
        {
            move_domain restored;
            require(restored.prepare(backend, files.paths, 16, "AUTO").empty()
                && restored.bind().empty() &&
                restored.identity() == identity && restored.path() == path,
                "automatic journal restart identity");
        }
        {
            move_domain explicit_path;
            require(explicit_path.prepare(backend, files.paths, 16, path).
                empty() && explicit_path.bind().empty(),
                "explicit path cannot resume automatic journal");
        }
        move_domain disabled;
        require(!disabled.prepare(backend, files.paths, 16, "off").empty(),
            "bound automatic journal was silently disabled");
    }
    scratch files;
    move_domain memory;
    require(memory.prepare("mdb", files.paths, 16, "auto").empty() &&
        !memory.enabled() && memory.bind().empty() && !memory.journal() &&
        ::access((files.root + "/a").c_str(), F_OK),
        "memory automatic journal created storage");
}
static void path_aliases() {
    scratch files;
    size_t number = 0;
    for (const auto* suffix : {" space", ";part", ",part", "\tpart",
        "\npart", "\rpart"}) {
        const auto actual = files.root + "/actual" + suffix;
        const auto alias = files.root + "/alias-" + std::to_string(number++);
        require(!acl_make_dirs(actual.c_str(), 0755) &&
            !::symlink(actual.c_str(), alias.c_str()), "path alias seed");
        std::string identity, path;
        {
            move_domain domain;
            require(domain.prepare("rdb", alias, 16, "auto").empty() &&
                domain.roots().size() == 1 && domain.bind().empty(),
                "canonical separator path rejected");
            path = domain.path(); identity = domain.identity();
            require(path == domain.roots().front() + "/.pkv-move.decision",
                "canonical journal path changed");
        }
        move_domain restored;
        require(restored.prepare("rdb", alias, 16, "auto").empty() &&
            restored.identity() == identity && restored.path() == path &&
            restored.bind().empty(), "canonical path identity changed");
    }
}
int main() {
    acl::acl_cpp_init();
    lifecycle(); configuration(); damaged_and_partial(); io_faults();
    existing_marker_faults(); automatic(); path_aliases();
    std::cout << "MOVE domain identity, ownership "
        "and binding failures passed\n";
}
