#include "move_journal_internal.h"
#include <new>

namespace pkv {
move_journal::move_journal(move_journal_io* operations)
: data_(new implementation(operations)) {}
move_journal::~move_journal() = default;
move_journal::status move_journal::state() const { return data_->current; }
uint64_t move_journal::last_serial() const {
    return data_->decision.serial;
}
const move_decision* move_journal::pending() const {
    return state() == status::pending ? &data_->decision : nullptr;
}
move_journal::status move_journal::open(const std::string& path,
    const std::string& identity) {
    if (state() != status::unopened) return status::failed;
    auto& value = *data_;
    value.current = status::failed;
    if (path.empty() || path[0] != '/' || path.back() == '/' ||
        path.find('\0') != std::string::npos || identity.empty() ||
        identity.size() > 65536) return state();
    try {
        value.path = path; value.identity = identity;
        const auto parent = path.substr(0, path.find_last_of('/') + 1);
        value.directory.fd = value.io.directory(parent.c_str());
        if (value.directory.fd < 0) return state();
        const auto lock_path = path + ".lock";
        value.lock.fd = value.io.acquire(lock_path.c_str());
        if (value.lock.fd < 0 || !value.load())
            value.current = status::failed;
    } catch (const std::bad_alloc&) { value.current = status::failed; }
    return state();
}
bool move_journal::decide(const move_decision& decision) {
    if (state() != status::empty ||
        decision.serial <= data_->decision.serial) return false;
    data_->current = status::failed;
    try { return data_->save(decision); }
    catch (const std::bad_alloc&) { return false; }
}
bool move_journal::complete(uint64_t serial) {
    if (state() != status::pending || data_->decision.serial != serial)
        return false;
    data_->current = status::failed;
    if (data_->io.remove(data_->path.c_str()) ||
        !data_->sync(data_->directory.fd)) return false;
    data_->decision.owner.clear(); data_->current = status::empty;
    return true;
}
} // namespace pkv
