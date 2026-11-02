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
    for (uint64_t i = 0; i < root_.pages; ++i) {
        std::string bytes;
        if (!read_page(i, bytes)) {
            destination.transaction_.abort(); return false;
        }
        if (!destination.write_page(i, bytes)) {
            transaction_.abort(); return false;
        }
    }
    destination.root_ = root_;
    copied = true;
    return true;
}

} // namespace pkv
