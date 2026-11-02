#include "common/configuration.h"
#include "action/redis_service.h"
#include <cstdlib>
#include <iostream>
#include <new>

static thread_local long budget = -1;
void* operator new(std::size_t size) {
    if (!budget) throw std::bad_alloc();
    if (budget > 0) --budget;
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
#if __cplusplus >= 201402L
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
#endif
using namespace pkv;
static void require(bool ok, const char* reason) {
    if (!ok) { std::cerr << reason << '\n'; std::exit(1); }
}
static std::string prepare(const std::vector<std::string>& values,
    config_change& change) {
    std::vector<const std::string*> pairs;
    for (const auto& value : values) pairs.push_back(&value);
    return config_prepare(pairs, change);
}
static void parser() {
    config_change change;
    require(prepare({"requirepass", "secret", "acllog-max-len", "2"},
        change).empty() && change.password && change.audit &&
        change.value == "secret" && change.limit == 2, "complete plan");
    for (const auto& args : std::vector<std::vector<std::string>>{
        {}, {"requirepass"}, {"unknown", "1"}, {"databases", "16"},
        {"aclfile", "new"}, {"cluster-enabled", "no"},
        {"requirepass", "a", "requirepass", "b"},
        {"acllog-max-len", "-1"}, {"acllog-max-len", "01"},
        {"acllog-max-len", "9223372036854775808"},
        {"appendonly", "yes"}, {"save", "60 1"}}) {
        require(!prepare(args, change).empty() && change.value == "secret" &&
            change.limit == 2, "invalid plan changed staged state");
    }
    require(prepare({"appendonly", "nO", "save", ""}, change).empty(),
        "disabled persistence no-op");
    require(prepare({"replica-read-only", "no", "slave-read-only", "yes"},
        change).empty() && change.replica_policy && change.replica_read_only,
        "replica aliases did not retain final value");
    require(config_matches("ACL*", "acllog-max-len") &&
        config_matches("ACLLOG-MAX-LEN", "acllog-max-len") &&
        config_matches("[aA]cl?ile", "aclfile"), "glob/exact case");
}
static void prepare_failures() {
    const std::string name("requirepass"), secret(4096, 'x');
    const std::string capacity("acllog-max-len"), two("2");
    const std::vector<const std::string*> args{&name, &secret, &capacity, &two};
    size_t failed = 0, passed = 0;
    for (long n = 0; n < 32; ++n) {
        config_change change;
        change.password = true; change.audit = true;
        change.value = "old"; change.limit = 31;
        bool oom = false; budget = n;
        try { require(config_prepare(args, change).empty(), "prepare plan"); }
        catch (const std::bad_alloc&) { oom = true; }
        budget = -1;
        if (oom) {
            ++failed;
            require(change.value == "old" && change.limit == 31,
                "partial prepared plan");
        } else {
            ++passed;
            require(change.value == secret && change.limit == 2,
                "incomplete prepared plan");
        }
    }
    require(failed && passed, "prepare OOM coverage");
}
static void passwords() {
    redis_service service;
    service.password("start");
    const auto identity = service.permissions().find("default");
    const std::vector<std::string> rules{"off", "resetpass", ">acl-secret",
        "-@all", "+get", "resetkeys", "~own:*"};
    std::vector<const std::string*> borrowed;
    for (const auto& rule : rules) borrowed.push_back(&rule);
    require(service.permissions().set("default", borrowed).empty(), "ACL edit");
    const auto previous = identity->snapshot();
    config_change change;
    require(prepare({"requirepass", "start"}, change).empty() &&
        service.configure(change).empty() && identity->snapshot() == previous,
        "unchanged config reset ACL password");
    const std::string secret("new\0secret", 10);
    require(prepare({"requirepass", secret, "acllog-max-len", "0"},
        change).empty() && service.configure(change).empty(), "apply config");
    const auto settings = identity->snapshot();
    require(!settings->enabled && !settings->nopass &&
        settings->passwords.size() == 1 && settings->passwords[0] ==
        acl_password_hash(secret) && settings->selectors[0].commands.size() == 1
        && settings->selectors[0].commands.count("get") &&
        settings->selectors[0].keys[0].text == "own:*", "reset permissions");
    require(service.config_password() == secret && !service.audit().enabled()
        && service.passwords().accepts("default", "start") &&
        !service.passwords().accepts("default", secret), "RPC policy changed");
}
static void failures() {
    size_t failed = 0, passed = 0;
    for (long n = 0; n < 512; ++n) {
        redis_service service; service.password("original");
        const auto before = service.permissions().find("default")->snapshot();
        config_change change;
        require(prepare({"requirepass", std::string(4096, 'x'),
            "acllog-max-len", "0"}, change).empty(), "failure plan");
        bool oom = false; budget = n;
        try { require(service.configure(change).empty(), "valid apply"); }
        catch (const std::bad_alloc&) { oom = true; }
        budget = -1;
        if (oom) {
            ++failed;
            require(service.config_password() == "original" &&
                service.audit().limit() == 128 && service.permissions().find(
                    "default")->snapshot() == before, "partial OOM commit");
        } else {
            ++passed;
            require(service.config_password() == std::string(4096, 'x') &&
                service.audit().limit() == 0, "missing committed config");
        }
    }
    require(failed && passed, "OOM coverage");
}
int main() {
    parser(); prepare_failures(); passwords(); failures();
    std::cout << "CONFIG staged validation, password semantics and "
        "OOM passed\n";
}
