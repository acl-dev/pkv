#include "stdafx.h"
#include "common/command_catalog.h"
#include "common/command_keys.h"
#include "common/geo_radius.h"
#include <cstdlib>
#include <iostream>

using namespace pkv;
static void require(bool ok, const char* reason) {
    if (!ok) { std::cerr << reason << '\n'; std::exit(1); }
}
static std::vector<const std::string*> borrow(
    const std::vector<std::string>& values) {
    std::vector<const std::string*> out;
    for (const auto& value : values) out.push_back(&value);
    return out;
}
static void radius() {
    for (const std::string name : {"GEORADIUS", "GEORADIUSBYMEMBER"}) {
        std::vector<std::string> values{name, "source"};
        if (name == "GEORADIUS") values.insert(values.end(), {"0", "0"});
        else values.push_back("member");
        values.insert(values.end(), {"10", "km", "STORE", "first",
            "STOREDIST", "dist", "STORE", "last"});
        const auto args = borrow(values);
        const auto* entry = command_catalog(name);
        require(entry, "radius catalog missing");
        std::vector<command_key_argument> keys;
        require(command_key_arguments(*entry, args, keys) && keys.size() == 3 &&
            *keys[0].key == "source" && *keys[1].key == "first" &&
            *keys[2].key == "dist", "keyword metadata did not use firsts");
        require(command_key_arguments(*entry, args, keys, true) &&
            keys.size() == 4 && *keys.back().key == "last" &&
            keys.back().flags == "OW update", "actual target not authorized");
        auto effective = args;
        require(command_keys(effective) && effective.size() == 4 &&
            *effective[0] == "source" && *effective[1] == "first" &&
            *effective[2] == "dist" && *effective[3] == "last",
            "radius routing omitted metadata or actual destination");
        values[0] += "_RO";
        effective = borrow(values);
        require(command_keys(effective) && effective.size() == 1 &&
            *effective[0] == "source", "readonly radius routed a target");
    }
    std::vector<std::string> aliased{"GEORADIUS", "source", "0", "0", "1",
        "m", "STORE", "first", "STORE", "source"};
    auto interned = borrow(aliased);
    interned.back() = interned[1];
    std::vector<command_key_argument> authorized;
    require(command_key_arguments(*command_catalog("GEORADIUS"), interned,
        authorized, true) && authorized.size() == 3 &&
        authorized.back().flags == "OW update",
        "shared key pointer suppressed write authorization");
    std::vector<std::string> values{"GEORADIUS", "key", "0", "0", "1",
        "STORE", "unit-target"};
    require(radius_destination(borrow(values)) == 0,
        "unit value was mistaken for a destination option");
    auto effective = borrow(values);
    require(command_keys(effective) && effective.size() == 1 &&
        *effective[0] == "key", "unit was mistaken for a routing key");
}
static void keywords() {
    command_key_rule rule{"RO access", -1, false, 0, 0, 1, "STORE"};
    command_catalog_entry entry{"test", -2, "@read", nullptr, 0, nullptr, 0,
        &rule, 1};
    std::vector<std::string> values{"test", "source", "STORE", "first",
        "STORE", "last"};
    std::vector<command_key_argument> keys;
    require(command_key_arguments(entry, borrow(values), keys) &&
        keys.size() == 1 && *keys[0].key == "last", "backward keyword search");
    rule.begin = 2;
    require(command_key_arguments(entry, borrow(values), keys) &&
        keys.size() == 1 && *keys[0].key == "first", "forward keyword search");
    values = {"test", "source", "STORE"};
    require(command_key_arguments(entry, borrow(values), keys) && keys.empty(),
        "forward scan treated final keyword as a key");
    rule.begin = -1;
    require(!command_key_arguments(entry, borrow(values), keys),
        "reverse scan accepted dangling key");
    values = {"test", "source", "other", "value"};
    require(command_key_arguments(entry, borrow(values), keys) && keys.empty(),
        "absent optional keyword failed");
}
int main() {
    radius(); keywords();
    std::cout << "Radius keyword metadata, effective routing and ACL keys "
        "passed\n";
}
