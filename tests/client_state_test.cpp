#include "common/client_state.h"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <new>
#include <set>
#include <thread>
#include <type_traits>
#include <vector>

static thread_local long budget = -1;
static thread_local size_t attempts = 0;
void* operator new(std::size_t size) {
    ++attempts;
    if (!budget) throw std::bad_alloc();
    if (budget > 0) --budget;
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
#if __cplusplus >= 201402L
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }
#endif
using namespace pkv;
static_assert(!std::is_copy_constructible<client_state>::value,
    "Connection identifiers cannot be copied");
static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
static void metadata() {
    client_state first, second;
    using status = client_state::status;
    require(first.id() > 0 && second.id() > first.id() &&
        first.name().empty() && first.library_name().empty(), "client init");
    require(first.set_name("connection") == status::ok &&
        first.set_library(false, "library") == status::ok &&
        first.set_library(true, "1.0") == status::ok &&
        second.name().empty(), "metadata isolation");
    require(first.protocol() == 2 && second.protocol() == 2,
        "default protocol");
    first.authenticated(false);
    require(!first.authenticated() && second.authenticated(),
        "authentication isolation");
    first.protocol(3);
    require(first.protocol() == 3 && second.protocol() == 2,
        "protocol isolation");
    const auto id = first.id();
    first.reset();
    require(first.id() == id && first.protocol() == 2 &&
        first.authenticated() &&
        first.name().empty() &&
        first.library_name() == "library" && first.library_version() == "1.0",
        "RESET cleared stable connection attributes");
    first.reset(false);
    require(!first.authenticated() && first.id() == id &&
        first.protocol() == 2 && first.library_name() == "library",
        "protected RESET authentication");
    require(first.set_library(false, "") == status::ok &&
        first.set_library(true, "") == status::ok &&
        first.library_name().empty() && first.library_version().empty(),
        "empty library metadata was not cleared");
    for (unsigned i = 0; i < 256; ++i) {
        const std::string value(1, static_cast<char>(i));
        const bool valid = i >= 33 && i <= 126;
        require(first.set_name("stable") == status::ok,
            "validation baseline");
        require((first.set_name(value) == status::ok) == valid &&
            first.name() == (valid ? value : "stable"),
            "client name byte validation");
        require(first.set_library(true, "stable") == status::ok &&
            (first.set_library(true, value) == status::ok) == valid &&
            first.library_version() == (valid ? value : "stable"),
            "library byte validation");
    }
    require(first.set_name("") == status::ok && first.name().empty(),
        "empty name was not cleared");
}
static void allocations() {
    client_state state;
    using status = client_state::status;
    const std::string large(70000, 'x');
    require(state.set_name("old") == status::ok &&
        state.set_library(false, "lib") == status::ok &&
        state.set_library(true, "ver") == status::ok, "allocation baseline");
    budget = 0;
    attempts = 0;
    const auto name = state.set_name(large);
    const auto name_attempts = attempts;
    attempts = 0;
    const auto library = state.set_library(false, large);
    const auto library_attempts = attempts;
    attempts = 0;
    const auto version = state.set_library(true, large);
    const auto version_attempts = attempts;
    const bool preserved = state.name() == "old" &&
        state.library_name() == "lib" && state.library_version() == "ver";
    state.reset();
    budget = -1;
    if (!preserved || name != status::no_memory ||
        library != status::no_memory || version != status::no_memory ||
        !name_attempts || !library_attempts || !version_attempts)
        std::cerr << "OOM states=" << int(name) << ',' << int(library) << ',' <<
            int(version) << "; hooks=" << name_attempts << ',' <<
            library_attempts << ',' << version_attempts <<
            "; preserved=" << preserved << '\n';
    require(name_attempts && library_attempts && version_attempts &&
        preserved && name == status::no_memory &&
        library == status::no_memory &&
        version == status::no_memory && state.name().empty() &&
        state.library_name() == "lib" && state.library_version() == "ver",
        "failed metadata allocation changed state");
    require(state.set_name(large) == status::ok && state.name() == large &&
        state.set_name(state.name()) == status::ok && state.name() == large,
        "owned metadata or self-alias update failed");
    state.reset();
    require(state.name().empty(), "large name reset");
}
static void identifiers() {
    std::vector<std::vector<long long>> ids(8);
    std::vector<std::thread> workers;
    for (size_t t = 0; t < ids.size(); ++t) workers.emplace_back([&, t] {
        ids[t].reserve(1000);
        for (unsigned i = 0; i < 1000; ++i) {
            client_state state;
            const auto id = state.id();
            state.reset();
            require(id > 0 && state.id() == id &&
                (ids[t].empty() || id > ids[t].back()), "client ID order");
            ids[t].push_back(id);
        }
    });
    for (auto& worker : workers) worker.join();
    std::set<long long> unique;
    for (const auto& group : ids)
        for (auto id : group) require(unique.insert(id).second,
            "duplicate client identifiers");
    require(unique.size() == 8000, "lost client identifiers");
}
static void commands() {
    for (const auto& row : {std::make_pair("ID", 2),
        std::make_pair("gEtNaMe", 2),
        std::make_pair("SETNAME", 3),
        std::make_pair("SETINFO", 4),
        std::make_pair("KILL", -3),
        std::make_pair("HELP", 2)}) {
        const auto* spec = client_subcommand(row.first);
        require(spec && spec->arity == row.second, "client command table");
    }
    require(client_subcommand("kill")->valid_arity(3) &&
        client_subcommand("kill")->valid_arity(100) &&
        !client_subcommand("kill")->valid_arity(2), "variable CLIENT arity");
    require(!client_subcommand("unknown") &&
        !client_subcommand(std::string("ID\0x", 4)), "client token truncation");
}
int main() {
    metadata(); allocations(); identifiers(); commands();
    std::cout << "Connection metadata, ID and allocation checks passed\n";
}
