#include "list_store.h"

namespace pkv {

bool list_store::move_to(list_store& destination, bool from_left, bool to_left,
    std::string& value, bool& found) {
    found = false;
    if (error_ != status::ok) {
        transaction_.abort();
        destination.transaction_.abort();
        return false;
    }
    if (&transaction_ != &destination.transaction_) {
        destination.transaction_.abort();
        return fail(status::invalid_argument);
    }
    if (!root_.elements) return true;
    if (destination.error_ != status::ok) {
        transaction_.abort();
        return false;
    }
    if (owner_ == destination.owner_) {
        if (from_left == to_left || root_.elements == 1) {
            if (!index(from_left ? 0 : -1, value, found)) return false;
            dirty_ = found;
            return true;
        }
        std::vector<std::string> moved;
        if (!pop(1, from_left, &moved) || !push(moved, to_left)) return false;
        value = std::move(moved.front());
        found = true;
        return true;
    }
    std::vector<std::string> moved;
    if (!pop(1, from_left, &moved) || !destination.push(moved, to_left)) {
        transaction_.abort();
        return false;
    }
    value = std::move(moved.front());
    found = true;
    return true;
}

bool list_store::clone_to(list_store& destination, bool& copied) {
    copied = false;
    if (error_ != status::ok || destination.error_ != status::ok) {
        destination.transaction_.abort();
        return false;
    }
    if (&transaction_ == &destination.transaction_ &&
        owner_ == destination.owner_) return fail(status::invalid_argument);
    if (!root_.elements) return true;
    destination.initialize(root_.expires_at_ms);
    if (destination.error_ != status::ok) return false;
    destination.root_ = root_;
    std::unordered_set<uint64_t> visited;
    uint64_t id = root_.head, previous = 0, members = 0, offset = 0, total = 0;
    while (id) {
        list_page page;
        if (!visited.insert(id).second || !read_page(id, page) ||
            page.previous != previous) {
            destination.transaction_.abort();
            return fail(status::invalid);
        }
        for (const auto& item : page.fragments) {
            if (offset == 0) total = item.total_bytes;
            if (item.total_bytes != total || item.offset != offset) {
                destination.transaction_.abort();
                return fail(status::invalid);
            }
            offset += item.bytes.size();
            if (offset == total) { ++members; offset = 0; }
            if (members > root_.elements) {
                destination.transaction_.abort();
                return fail(status::invalid);
            }
        }
        if (!destination.write_page(page)) return false;
        previous = id;
        id = page.next;
    }
    if (previous != root_.tail || members != root_.elements || offset != 0) {
        destination.transaction_.abort();
        return fail(status::invalid);
    }
    copied = true;
    return true;
}

} // namespace pkv
