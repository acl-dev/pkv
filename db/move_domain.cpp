#include "stdafx.h"
#include "move_domain_internal.h"
#include "rpc/page_wire.h"
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <stdexcept>
#include <zlib.h>

namespace pkv {
move_domain::implementation::~implementation() {
    journal.reset();
    for (const int fd : locks) io.close(fd);
}
move_domain::move_domain(move_journal_io* operations)
: data_(new implementation(operations)) {}
move_domain::~move_domain() = default;
bool move_domain::enabled() const {
    return data_->prepared && !data_->failed && !data_->path.empty();
}
const std::vector<std::string>& move_domain::roots() const {
    return data_->roots;
}
const std::string& move_domain::path() const { return data_->path; }
const std::string& move_domain::identity() const { return data_->identity; }
move_journal* move_domain::journal() {
    return data_->bound && !data_->failed ? data_->journal.get() : nullptr;
}
std::string move_domain::prepare(const std::string& backend,
    const std::string& paths, size_t databases, const std::string& journal) {
    if (data_->attempted) return "move domain already initialized";
    data_->attempted = true;
    try {
        data_->prepare(backend, paths, databases, journal);
        data_->prepared = true; return {};
    } catch (const std::exception& error) {
        data_->failed = true; return error.what();
    }
}
std::string move_domain::bind() {
    if (!data_->prepared || data_->failed) return "move domain not prepared";
    if (data_->bound) return "move domain already bound";
    try { data_->bind(); data_->bound = true; return {}; }
    catch (const std::exception& error) {
        data_->failed = true; return error.what();
    }
}
void move_domain::implementation::prepare(const std::string& backend,
    const std::string& paths, size_t databases, const std::string& log) {
    const bool disk = !strcasecmp(backend.c_str(), "rdb") ||
        !strcasecmp(backend.c_str(), "wdb");
    const bool automatic = !strcasecmp(log.c_str(), "auto");
    const bool disabled = log.empty() || !strcasecmp(log.c_str(), "off");
    if (!disk) {
        if (!automatic && !disabled) throw std::runtime_error(
            "move_journal_path requires a native disk backend");
        return;
    }
    if (!databases || databases > 65536 ||
        paths.find('\0') != paths.npos)
        throw std::runtime_error("invalid move domain configuration");
    acl::string input(paths.c_str());
    for (const auto& base : input.split2(";, \t"))
        roots.emplace_back(base);
    if (roots.empty()) throw std::runtime_error("empty native storage paths");
    if (disabled) {
        for (const auto& root : roots) if (read_marker(root, false))
            throw std::runtime_error(
                "bound storage requires move_journal_path");
        return;
    }
    for (auto& root : roots) {
        if (acl_make_dirs(root.c_str(), 0755))
            throw std::runtime_error("cannot create move domain directory");
        root = canonical(root);
    }
    path = canonical_journal(automatic ?
        roots.front() + "/.pkv-move.decision" : log);
    for (const auto& root : roots) {
        if (path == root) throw std::runtime_error("journal is a storage root");
        const auto prefix = root == "/" ? root : root + "/";
        if (path.compare(0, prefix.size(), prefix)) continue;
        const auto first = path.substr(prefix.size(),
            path.find('/', prefix.size()) - prefix.size());
        if (first == "rdb" || first == "wdb" ||
            first.compare(0, 11, "logical-db-") == 0 ||
            first == ".pkv-move.domain" || first == ".pkv-move.domain.lock")
            throw std::runtime_error("journal conflicts with managed storage");
    }
    encode(!strcasecmp(backend.c_str(), "rdb") ? "rdb" : "wdb", databases);
    validate(); lock_roots(); validate();
    journal.reset(new move_journal(&io));
    if (journal->open(path, identity) == move_journal::status::failed)
        throw std::runtime_error("cannot open or validate MOVE journal");
}
void move_domain::implementation::lock_roots() {
    auto sorted = roots;
    std::sort(sorted.begin(), sorted.end());
    if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end())
        throw std::runtime_error("duplicate canonical storage root");
    locks.reserve(sorted.size());
    for (const auto& root : sorted) {
        const auto name = root + "/.pkv-move.domain.lock";
        const int fd = io.acquire(name.c_str());
        if (fd < 0) throw std::runtime_error("move domain already owned");
        locks.push_back(fd);
    }
}
void move_domain::implementation::validate() {
    for (const auto& root : roots) read_marker(root, true);
}
void move_domain::implementation::bind() {
    if (path.empty()) return;
    validate();
    for (const auto& root : roots) {
        if (read_marker(root, true, true)) continue;
        const auto result = acl_save_file(root + "/.pkv-move.domain", marker,
            &io);
        if (!result.ok()) throw std::runtime_error(result.error);
    }
}
void move_domain::implementation::encode(const std::string& backend,
    size_t databases) {
    identity.assign("PKVDOM1\0", 8);
    auto append = [&](const std::string& bytes) {
        if (bytes.size() > 65536 || identity.size() + bytes.size() + 4 > 65532)
            throw std::runtime_error("move domain identity too large");
        char size[4];
        page_wire::put32(size, static_cast<uint32_t>(bytes.size()));
        identity.append(size, 4); identity += bytes;
    };
    append(backend);
    char number[8];
    page_wire::put32(number, static_cast<uint32_t>(databases));
    page_wire::put32(number + 4, static_cast<uint32_t>(roots.size()));
    identity.append(number, 8);
    for (const auto& root : roots) append(root);
    append(path); marker = identity;
    char crc[4];
    page_wire::put32(crc, static_cast<uint32_t>(::crc32(0,
        reinterpret_cast<const Bytef*>(identity.data()), identity.size())));
    marker.append(crc, 4);
}
} // namespace pkv
