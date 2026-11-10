#include "string_store.h"

namespace pkv {

bool string_store::clone_to(string_store& destination, bool& copied) {
    copied = false;
    if (error_ != status::ok || destination.error_ != status::ok) {
        transaction_.abort(); destination.transaction_.abort(); return false;
    }
    if (&transaction_ == &destination.transaction_ &&
        owner_ == destination.owner_) return fail(status::invalid_argument);
    if (!exists_) return true;
    destination.initialize(root_.expires_at_ms);
    if (destination.error_ != status::ok) return false;
    if (root_.pages > 1)
        destination.transaction_.reserve_sorted_pages(destination.entry_,
            static_cast<size_t>(root_.pages));
    for (uint64_t i = 0; i < root_.pages; ++i) {
        std::string bytes;
        if (!read_page(i, bytes)) {
            destination.transaction_.abort(); return false;
        }
        if (!destination.transaction_.put_tail_bound(destination.entry_, i + 1,
                std::move(bytes))) {
            transaction_.abort(); return destination.fail(status::storage);
        }
    }
    destination.root_ = root_;
    copied = true;
    return true;
}

} // namespace pkv
