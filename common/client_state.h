#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace pkv {
struct client_command_spec {
    const char* name;
    int arity;
    const char* full_name;
    bool valid_arity(size_t count) const {
        return arity < 0 ? count >= static_cast<size_t>(-arity) :
            count == static_cast<size_t>(arity);
    }
};
struct client_snapshot {
    long long id = 0;
    unsigned protocol = 2;
    size_t heap_bytes = 0;
    std::string name, library_name, library_version, username;
    uint64_t user_id = 1;
};
const client_command_spec* client_subcommand(const std::string& name);

// Commands execute on the owning connection. Reference getters are actor-
// only; other threads use a copied snapshot.
class client_state {
public:
    enum class status { ok, invalid, no_memory };
    client_state();
    client_state(const client_state&) = delete;
    client_state& operator=(const client_state&) = delete;
    long long id() const { return id_; }
    bool authenticated() const { return authenticated_; }
    void authenticated(bool value) { authenticated_ = value; }
    unsigned protocol() const { return protocol_; }
    void protocol(unsigned value) {
        std::lock_guard<std::mutex> lock(mutex_);
        protocol_ = value;
    }
    client_snapshot snapshot() const;
    status hello(unsigned protocol, const std::string* name,
        const std::string* user = nullptr, uint64_t user_id = 1);
    status login(const std::string& user, uint64_t user_id);
    const std::string& username() const;
    uint64_t user_id() const { return user_id_; }
    const std::string& name() const { return name_; }
    const std::string& library_name() const { return library_name_; }
    const std::string& library_version() const { return library_version_; }
    status set_name(const std::string& value) { return assign(name_, value); }
    status set_library(bool version, const std::string& value) {
        return assign(version ? library_version_ : library_name_, value);
    }
    void reset(bool authenticated = true) noexcept;
    static bool valid_metadata(const std::string& value);
private:
    const long long id_;
    unsigned protocol_ = 2;
    bool authenticated_ = true;
    uint64_t user_id_ = 1;
    std::shared_ptr<const std::string> username_;
    std::string name_, library_name_, library_version_;
    mutable std::mutex mutex_;
    status assign(std::string& target, const std::string& value);
};
} // namespace pkv
