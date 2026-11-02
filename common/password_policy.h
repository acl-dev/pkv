#pragma once
#include <cstddef>
#include <string>
#include <utility>

namespace pkv {
// Configured once before worker threads start. Ordinary commands only consult
// their connection's authenticated flag; no shared authentication lock is used.
class password_policy {
public:
    explicit password_policy(std::string password = {})
        : password_(std::move(password)) {}
    bool protected_server() const { return !password_.empty(); }
    size_t password_size() const { return password_.size(); }
    bool accepts(const std::string& user, const std::string& password) const;
    // Compare a bounded fragment without exposing or copying the secret.
    bool matches_fragment(size_t offset, const char* bytes, size_t size) const;
private:
    std::string password_;
};
} // namespace pkv
