#include "common/acl_policy.h"
#include "common/acl_internal.h"
#include "common/client_state.h"
#include <new>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <thread>

static thread_local long allocations = -1;
void* operator new(std::size_t size) {
    if (!allocations) throw std::bad_alloc();
    if (allocations > 0) --allocations;
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
#if __cplusplus >= 201402L
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
#endif
using namespace pkv;
static void require(bool ok, const char* reason) {
    if (!ok) { std::cerr << reason << '\n'; std::exit(1); }
}
static std::string set(acl_policy& policy, const std::string& user,
    const std::vector<std::string>& rules) {
    std::vector<const std::string*> borrowed;
    for (const auto& rule : rules) borrowed.push_back(&rule);
    return policy.set(user, borrowed);
}
static acl_decision check(acl_policy& policy, const std::string& user,
    const std::vector<std::string>& args) {
    std::vector<const std::string*> borrowed;
    for (const auto& arg : args) borrowed.push_back(&arg);
    return policy.check(*policy.find(user), borrowed);
}
static void digests() {
    require(acl_password_hash("") ==
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        "empty SHA-256 vector");
    require(acl_password_hash("abc") ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "abc SHA-256 vector");
    require(acl_password_hash(std::string(1000000, 'a')) ==
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
        "million-a SHA-256 vector");
    require(acl_password_hash("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmno"
        "mnopnopq") ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
        "multi-block SHA-256 vector");
    require(acl_secret_equal("same", "same") &&
        !acl_secret_equal("same", "sAme") && !acl_secret_equal("", "x"),
        "secret comparison");
}
static void credentials() {
    acl_policy policy("root");
    require(!policy.default_login() && policy.authenticate("default", "root")
        && !policy.authenticate("default", "bad"), "configured default");
    require(set(policy, "bob", {"on", ">password", ">other"}).empty(),
        "user create");
    auto identity = policy.authenticate("bob", "password");
    require(identity && policy.authenticate("bob", "other") &&
        !policy.authenticate("missing", "password"), "password alternatives");
    require(set(policy, "bob", {"off"}).empty() &&
        !policy.authenticate("bob", "password"), "off blocks new logins");
    require(set(policy, "bob", {"on", "nopass"}).empty() &&
        policy.authenticate("bob", std::string("binary\0pass", 11)), "nopass");
    const auto prior = identity->snapshot();
    require(!set(policy, "bob", {">new", "#ABC"}).empty() &&
        identity->snapshot() == prior, "failed rules partially published");
    require(!set(policy, "bad user", {"on"}).empty() &&
        !policy.find("bad user"), "invalid user published");
    const std::string name("bob");
    const auto removed = policy.remove({&name, &name});
    require(removed.size() == 1 && identity->revoked() && !policy.find(name),
        "identity revocation");
    require(set(policy, name, {"on", "nopass"}).empty() &&
        policy.find(name)->id != identity->id && identity->revoked(),
        "same-name identity revived");
}
static void permissions() {
    acl_policy policy;
    require(policy.find("default")->unrestricted(), "default fast path");
    require(set(policy, "bob", {"on", "nopass", "+get", "+mget",
        "%R~source:*", "%W~target:*", "+copy", "+set"}).empty(), "rules");
    require(check(policy, "bob", {"GET", "source:a"}).allowed() &&
        check(policy, "bob", {"GET", "target:a"}).reason == acl_denial::key &&
        check(policy, "bob", {"SET", "target:a", "v"}).allowed(),
        "read/write patterns");
    require(check(policy, "bob", {"COPY", "source:a", "target:a"}).allowed()
        && check(policy, "bob", {"COPY", "target:a", "source:a"}).reason ==
        acl_denial::key, "source and destination permissions");
    require(check(policy, "bob", {"MGET", "source:a", "target:a"}).reason ==
        acl_denial::key && check(policy, "bob", {"DEL", "source:a"}).reason ==
        acl_denial::command, "all keys checked; command precedes key denial");
    require(set(policy, "bob", {"reset", "+get", "~one:*",
        "(+mget", "~two:*)"}).empty(), "split selector");
    require(check(policy, "bob", {"MGET", "two:a", "two:b"}).allowed() &&
        check(policy, "bob", {"MGET", "one:a", "two:b"}).reason ==
        acl_denial::key, "selectors must not union permissions");
    const auto old = policy.find("bob")->snapshot();
    require(!set(policy, "bob", {"on", "(+get"}).empty() &&
        policy.find("bob")->snapshot() == old, "unmatched selector commit");
    require(set(policy, "bob", {"reset", "+@string", "~*", "-set",
        "+client|id", "+select|0"}).empty(), "category/subcommand/first arg");
    require(check(policy, "bob", {"GET", "any"}).allowed() &&
        check(policy, "bob", {"SET", "any", "v"}).reason ==
        acl_denial::command && check(policy, "bob", {"CLIENT", "ID"}).allowed()
        && check(policy, "bob", {"CLIENT", "LIST"}).reason ==
        acl_denial::command && check(policy, "bob", {"SELECT", "0"}).allowed()
        && check(policy, "bob", {"SELECT", "1"}).reason == acl_denial::command,
        "command rule precedence");
    require(!set(policy, "bob", {"~extra"}).empty(), "pattern after allkeys");
    require(check(policy, "bob", {"AUTH", "any"}).allowed(), "NOAUTH command");
}
static void channels_and_descriptions() {
    acl_policy policy;
    require(set(policy, "bob", {"on", "nopass", "~public:*", "&events:*",
        "+@all", "-client", "+client|id", "(+set ~private:*)"}).empty(),
        "description setup");
    const auto settings = policy.find("bob")->snapshot();
    const auto& root = settings->selectors.front();
    require(acl_channel_allowed(root, "events:a", false) &&
        !acl_channel_allowed(root, "other:a", false) &&
        acl_channel_allowed(root, "events:*", true) &&
        !acl_channel_allowed(root, "events:a*", true), "channel patterns");
    require(acl_selector_commands(root).find("-client +client|id") !=
        std::string::npos, "root/subcommand description");
    require(acl_describe(*policy.find("bob"), *settings).find(
        "(-@all +set ~private:*)") != std::string::npos,
        "selector description");
}
static void roundtrip() {
    acl_policy policy;
    require(set(policy, "quoted\"user", {"on", "nopass", "+@all", "-client",
        "+client|id", "%R~*", "%W~*", "~extra", "&a\\b", "&quoted\"x",
        "(+get \"~quote\\\"key\")"}).empty(), "quoted policy");
    const auto identity = policy.find("quoted\"user");
    std::vector<std::string> tokens;
    require(acl_split(acl_describe(*identity, *identity->snapshot()), tokens),
        "description tokenization");
    require(tokens.size() > 3 && tokens[1] == identity->name,
        "quoted username");
    tokens.erase(tokens.begin(), tokens.begin() + 2);
    require(set(policy, "clone", tokens).empty(), "description replay");
    const auto cloned = policy.find("clone")->snapshot();
    require(cloned->selectors.size() == 2 &&
        cloned->selectors[1].keys.front().text == "quote\"key" &&
        cloned->selectors.front().channels ==
            identity->snapshot()->selectors.front().channels &&
        acl_channel_allowed(cloned->selectors.front(), "ab", false),
        "escaped selector or channel changed");
    for (const auto& command : std::vector<std::vector<std::string>>{
        {"GET", "any"}, {"SET", "any", "v"}, {"CLIENT", "ID"},
        {"CLIENT", "LIST"}, {"MGET", "a", "b"}})
        require(check(policy, identity->name, command).reason ==
            check(policy, "clone", command).reason, "roundtrip permissions");
    require(!set(policy, "clone", {"(\"\")"}).empty() &&
        !set(policy, "clone", {"(\"+get\"tail)"}).empty(),
        "bad selector quotes");
}
static void allocation_failures() {
    acl_policy policy;
    size_t failed = 0, passed = 0;
    for (long budget = 0; budget < 128; ++budget) {
        require(set(policy, "oom", {"reset", "on", "nopass", "+get",
            "~old:*"}).empty(), "OOM setup");
        const auto identity = policy.find("oom");
        const auto before = identity->snapshot();
        const std::vector<std::string> rules{"reset", "on", "nopass", "+set",
            "~new:*", "(+get ~other:*)"};
        std::vector<const std::string*> borrowed;
        for (const auto& rule : rules) borrowed.push_back(&rule);
        bool oom = false;
        allocations = budget;
        try { require(policy.set("oom", borrowed).empty(), "valid OOM rules"); }
        catch (const std::bad_alloc&) { oom = true; }
        allocations = -1;
        if (oom) {
            ++failed;
            require(identity->snapshot() == before && !identity->revoked(),
                "OOM published partial policy");
        } else ++passed;
    }
    require(failed && passed, "missing publication allocation coverage");
    const std::string name("oom");
    const auto identity = policy.find(name);
    bool oom = false;
    const std::vector<const std::string*> names{&name};
    allocations = 0;
    try { policy.remove(names); } catch (const std::bad_alloc&) { oom = true; }
    allocations = -1;
    require(oom && policy.find(name) == identity && !identity->revoked(),
        "OOM revoked a user before staging completed");
}
static void new_user_failures() {
    acl_policy policy;
    const std::string name("new-user");
    const std::vector<const std::string*> names{&name};
    const std::vector<std::string> rules{"on", "nopass", "+get", "~x:*"};
    std::vector<const std::string*> borrowed;
    for (const auto& rule : rules) borrowed.push_back(&rule);
    size_t failed = 0, passed = 0;
    for (long budget = 0; budget < 64; ++budget) {
        bool oom = false;
        allocations = budget;
        try { require(policy.set(name, borrowed).empty(), "new user rules"); }
        catch (const std::bad_alloc&) { oom = true; }
        allocations = -1;
        if (oom) {
            ++failed; require(!policy.find(name), "OOM published a new user");
        } else {
            ++passed; require(policy.find(name) != nullptr, "new user missing");
            policy.remove(names);
        }
    }
    require(failed && passed, "missing new-user allocation coverage");
}
static void identity_metadata() {
    client_state state;
    const std::string large(70000, 'x'), old("old");
    size_t failed = 0, passed = 0;
    for (long budget = 0; budget < 8; ++budget) {
        require(state.hello(2, &old, &old, 2) == client_state::status::ok,
            "identity metadata setup");
        allocations = budget;
        const auto changed = state.hello(3, &large, &large, 9);
        allocations = -1;
        const auto snapshot = state.snapshot();
        if (changed == client_state::status::no_memory) {
            ++failed;
            require(snapshot.protocol == 2 && snapshot.name == old &&
                snapshot.username == old && snapshot.user_id == 2,
                "OOM partially changed protocol/name/user identity");
        } else {
            ++passed;
            require(snapshot.protocol == 3 && snapshot.name == large &&
                snapshot.username == large && snapshot.user_id == 9,
                "HELLO identity metadata");
        }
    }
    require(failed && passed, "missing identity allocation coverage");
    state.reset(false);
    require(state.username() == "default" && state.user_id() == 1 &&
        !state.authenticated(), "RESET identity");
}
static void diagnostic_key() {
    acl_policy policy;
    require(set(policy, "diagnostic", {"on", "nopass", "+mget", "~one:*",
        "(+mget ~two:*)"}).empty(), "diagnostic selectors");
    const auto decision = check(policy, "diagnostic",
        {"MGET", "one:a", "two:b"});
    require(decision.reason == acl_denial::key && decision.object == "two:b",
        "selector diagnostic must choose the furthest failing key");
}
static void concurrent_updates() {
    acl_policy policy;
    require(set(policy, "bob", {"on", "nopass", "+get", "~one:*"}).empty(),
        "concurrency setup");
    auto identity = policy.find("bob");
    std::atomic<bool> done{false};
    std::vector<std::thread> readers;
    for (unsigned i = 0; i < 4; ++i) readers.emplace_back([&] {
        while (!done.load()) {
            const auto settings = identity->snapshot();
            const auto& root = settings->selectors.front();
            require(root.keys.size() == 1 && root.commands.size() == 1 &&
                ((root.keys[0].text == "one:*" && root.commands.count("get")) ||
                 (root.keys[0].text == "two:*" && root.commands.count("set"))),
                "torn policy publication");
        }
    });
    for (unsigned i = 0; i < 2000; ++i)
        require(set(policy, "bob", {"reset", "on", "nopass",
            i % 2 ? "+get" : "+set", i % 2 ? "~one:*" : "~two:*"}).empty(),
            "policy writer");
    done = true;
    for (auto& reader : readers) reader.join();
}
static void revoked_diagnostics() {
    acl_policy policy;
    const std::string username("revoked");
    require(set(policy, username,
        {"on", "nopass", "+@all", "~*", "&*"}).empty(),
        "revoked diagnostic setup");
    const auto identity = policy.find(username);
    const std::vector<const std::string*> users{&username};
    policy.remove(users);
    std::string command("SeT"), key("key"), value("value");
    std::vector<const std::string*> args{&command, &key, &value};
    auto decision = policy.check(*identity, args);
    require(decision.reason == acl_denial::revoked && decision.object == "set",
        "revoked command name lost");
    command = "ClIeNt"; key = "ID"; args.pop_back();
    decision = policy.check(*identity, args);
    require(decision.reason == acl_denial::revoked &&
        decision.object == "client|id", "revoked subcommand name lost");
    args.clear();
    require(policy.check(*identity, args).reason == acl_denial::revoked,
        "empty revoked command");
    std::cout << "ACL revoked command/subcommand diagnostics passed\n";
}
int main() {
    revoked_diagnostics(); digests(); credentials();
    permissions(); channels_and_descriptions();
    roundtrip(); allocation_failures(); new_user_failures();
    identity_metadata(); diagnostic_key(); concurrent_updates();
    std::cout << "ACL digests, staged rules, selectors, identities and "
        "concurrent policy publication passed\n";
}
