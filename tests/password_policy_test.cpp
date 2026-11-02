#include "common/password_policy.h"
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>

static thread_local bool reject = false;
void* operator new(std::size_t size) {
    if (reject) throw std::bad_alloc();
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
#if __cplusplus >= 201402L
void operator delete(void* p, std::size_t) noexcept {
    ::operator delete(p);
}
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }
#endif
static void require(bool ok, const char* reason) {
    if (!ok) { std::cerr << reason << '\n'; std::exit(1); }
}
int main() {
    pkv::password_policy open;
    require(!open.protected_server() && open.accepts("default", "") &&
        open.accepts("default", "anything") && !open.accepts("Default", ""),
        "default/no-password semantics");
    for (unsigned i = 0; i < 256; ++i) {
        std::string secret = "begin";
        secret += static_cast<char>(i); secret += "end";
        pkv::password_policy policy(secret);
        require(policy.protected_server() && policy.accepts("default", secret)
            && !policy.accepts(std::string("default\0x", 9), secret) &&
            !policy.accepts("default", secret + "x") &&
            !policy.accepts("default", secret.substr(1)), "binary matching");
        for (size_t at = 0; at < secret.size(); ++at) {
            auto wrong = secret; wrong[at] ^= 1;
            require(!policy.accepts("default", wrong), "byte mismatch");
        }
    }
    const std::string large(1000000, 'x'), user("default");
    pkv::password_policy policy(large);
    const std::string wrong(1000000, 'y');
    reject = true;
    const bool correct = policy.accepts(user, large);
    const bool incorrect = policy.accepts(user, wrong);
    bool fragments = true;
    for (size_t offset = 0; offset < large.size(); offset += 1000)
        fragments = policy.matches_fragment(offset, large.data() + offset,
            1000) && fragments;
    reject = false;
    require(correct && !incorrect && fragments, "comparison allocated");
    require(!policy.matches_fragment(std::numeric_limits<size_t>::max(),
        large.data(), 1) && !policy.matches_fragment(large.size(),
        large.data(), 1) && policy.matches_fragment(large.size(), nullptr, 0),
        "fragment bounds");
    std::cout << "Password policy binary and allocation checks passed\n";
}
