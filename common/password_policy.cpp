#include "password_policy.h"

namespace pkv {
bool password_policy::matches_fragment(size_t offset, const char* bytes,
    size_t size) const {
    if (offset > password_.size() || size > password_.size() - offset)
        return false;
    volatile unsigned char difference = 0;
    for (size_t i = 0; i < size; ++i)
        difference = static_cast<unsigned char>(difference |
            (static_cast<unsigned char>(password_[offset + i]) ^
             static_cast<unsigned char>(bytes[i])));
    return difference == 0;
}
bool password_policy::accepts(const std::string& user,
    const std::string& password) const {
    if (user != "default") return false;
    if (!protected_server()) return true;
    return password.size() == password_.size() &&
        matches_fragment(0, password.data(), password.size());
}
} // namespace pkv
