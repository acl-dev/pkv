// Test-only macOS interposer: widen ACL's partial hook publication window.
#include <dlfcn.h>
#include <cstring>
#include <ctime>

static void* delayed_lookup(void* handle, const char* name) {
    if (!std::strcmp(name, "kevent")) {
        const timespec gap = {0, 300000000};
        nanosleep(&gap, nullptr);
    }
    return dlsym(handle, name);
}

__attribute__((used, section("__DATA,__interpose")))
static const struct {
    const void* replacement;
    const void* original;
} hook = {reinterpret_cast<const void*>(delayed_lookup),
    reinterpret_cast<const void*>(dlsym)};
