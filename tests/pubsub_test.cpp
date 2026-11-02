#include "stdafx.h"
#include "common/pubsub.h"
#include "common/acl_policy.h"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <new>
#include <thread>

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
using kind = pubsub_kind;
static void require(bool ok, const char* reason) {
    if (!ok) { std::cerr << reason << '\n'; std::exit(1); }
}
static std::vector<const std::string*> borrowed(
    const std::vector<std::string>& names) {
    std::vector<const std::string*> result;
    for (const auto& name : names) result.push_back(&name);
    return result;
}
class client {
public:
    pubsub_broker& broker;
    std::vector<std::string> wire;
    unsigned closed = 0, denied = 0;
    std::shared_ptr<connection_output> output;
    pubsub_broker::subscriber endpoint;
    client(pubsub_broker& owner, uint64_t id, int protocol = 2,
        std::shared_ptr<acl_identity> identity = nullptr,
        size_t limit = 32ULL * 1024 * 1024) : broker(owner),
        output(std::make_shared<connection_output>(
            [this](const char* data, size_t size) {
                wire.emplace_back(data, size); return size;
            }, [](size_t, bool) {}, [this] { ++closed; }, limit)),
        endpoint(owner.attach(id, output, protocol, std::move(identity),
            [this](bool) { ++denied; })) {}
    ~client() { broker.retire(endpoint); output->stop(); }
    std::string change(kind type, const std::vector<std::string>& names,
        bool subscribe = true, bool wait = true) {
        auto reply = broker.change(endpoint, type, borrowed(names), subscribe);
        require(reply.barrier != 0, "missing subscription barrier");
        require(output->fulfil_reply(reply.barrier,
            std::make_shared<const std::string>(reply.acknowledgements)),
            "subscription fulfil failed");
        if (wait) require(output->wait(reply.barrier), "subscription reply");
        return reply.acknowledgements;
    }
    void flush() {
        const auto id = output->reserve_reply();
        require(id && output->fulfil_reply(id,
            std::make_shared<const std::string>()) && output->wait(id),
            "flush publication frames");
    }
};
static void encoding() {
    const std::string binary("a\0b", 3), payload("\0\r\n", 3);
    require(pubsub_ack(kind::channel, true, &binary, 1, 2) ==
        "*3\r\n$9\r\nsubscribe\r\n$3\r\n" + binary + "\r\n:1\r\n",
        "binary RESP2 acknowledgement");
    require(pubsub_ack(kind::shard, false, nullptr, 0, 3) ==
        ">3\r\n$12\r\nsunsubscribe\r\n_\r\n:0\r\n",
        "RESP3 empty shard acknowledgement");
    require(pubsub_ack(kind::pattern, false, nullptr, 2, 2) ==
        "*3\r\n$12\r\npunsubscribe\r\n$-1\r\n:2\r\n",
        "RESP2 empty pattern acknowledgement");
    require(pubsub_message(binary, payload, nullptr, true, 3) ==
        ">3\r\n$8\r\nsmessage\r\n$3\r\n" + binary + "\r\n$3\r\n" +
        payload + "\r\n", "binary RESP3 shard message");
    const std::string pattern("*");
    require(pubsub_message("a", "x", &pattern, false, 2) ==
        "*4\r\n$8\r\npmessage\r\n$1\r\n*\r\n$1\r\na\r\n$1\r\nx\r\n",
        "RESP2 pattern message");
}
static void membership() {
    go[] {
        pubsub_broker broker;
        client first(broker, 1), second(broker, 2, 3);
        first.output->start(); second.output->start();
        const auto ack = first.change(kind::channel, {"a", "a", "b"});
        require(ack == "*3\r\n$9\r\nsubscribe\r\n$1\r\na\r\n:1\r\n"
            "*3\r\n$9\r\nsubscribe\r\n$1\r\na\r\n:1\r\n"
            "*3\r\n$9\r\nsubscribe\r\n$1\r\nb\r\n:2\r\n",
            "duplicate subscribe acknowledgement counts");
        first.change(kind::pattern, {"a*", "*"});
        second.change(kind::pattern, {"a*"});
        first.change(kind::shard, {"a"});
        require(first.endpoint->counts().total() == 5 &&
            broker.counts().pattern == 2 && broker.counts().channel == 2 &&
            broker.counts().shard == 1,
            "subscription and unique pattern counts");
        first.wire.clear(); second.wire.clear();
        require(broker.publish(kind::channel, "a", "one") == 4 &&
            broker.publish(kind::shard, "a", "two") == 1,
            "publish delivery counts");
        first.flush(); second.flush();
        require(first.wire.size() == 4 && second.wire.size() == 1 &&
            second.wire[0][0] == '>' && first.wire[0][0] == '*',
            "pattern fanout, shard isolation or protocol types");
        const auto removed = first.change(kind::channel,
            {"a", "a", "missing"}, false);
        require(removed == "*3\r\n$11\r\nunsubscribe\r\n$1\r\na\r\n:3\r\n"
            "*3\r\n$11\r\nunsubscribe\r\n$1\r\na\r\n:3\r\n"
            "*3\r\n$11\r\nunsubscribe\r\n$7\r\nmissing\r\n:3\r\n",
            "duplicate unsubscribe counts");
        first.change(kind::pattern, {}, false);
        first.change(kind::shard, {}, false);
        const auto empty = first.change(kind::pattern, {}, false);
        require(empty == "*3\r\n$12\r\npunsubscribe\r\n$-1\r\n:1\r\n",
            "empty unsubscribe preserved classic family count");
        const auto id = broker.synchronize(first.endpoint, 2, nullptr, true);
        first.output->fulfil_reply(id,
            std::make_shared<const std::string>("+RESET\r\n"));
        require(first.output->wait(id) && !first.endpoint->counts().total() &&
            !first.endpoint->owned() && broker.counts().pattern == 1 &&
            !broker.counts().channel && !broker.counts().shard,
            "reset failed to release routes");
        broker.retire(second.endpoint); broker.retire(second.endpoint);
        require(!broker.counts().total(), "retire left channel indexes");
    };
    acl::fiber::schedule();
}
static void protocol_order() {
    go[] {
        pubsub_broker broker;
        client owner(broker, 7);
        owner.output->start(); owner.change(kind::channel, {"a"});
        owner.wire.clear();
        const auto reserved = owner.output->reserve_reply();
        broker.publish(kind::channel, "a", "old");
        require(broker.synchronize(owner.endpoint, 3, nullptr, false) ==
            reserved, "protocol switch lost pending reply");
        broker.publish(kind::channel, "a", "new");
        owner.output->fulfil_reply(reserved,
            std::make_shared<const std::string>("+HELLO\r\n"));
        require(owner.output->wait(reserved) && owner.wire.size() >= 2 &&
            owner.wire[0] ==
                "*3\r\n$7\r\nmessage\r\n$1\r\na\r\n$3\r\nold\r\n" &&
            owner.wire[1] == "+HELLO\r\n", "old push before protocol reply");
        owner.flush();
        require(owner.wire.size() == 3 && owner.wire[2] ==
            ">3\r\n$7\r\nmessage\r\n$1\r\na\r\n$3\r\nnew\r\n",
            "new push before protocol switch");
        owner.wire.clear();
        const auto subscribed = broker.change(owner.endpoint, kind::channel,
            borrowed({"b"}), true);
        broker.publish(kind::channel, "a", "self", 7);
        const auto removed = broker.change(owner.endpoint, kind::channel,
            borrowed({"a"}), false);
        require(subscribed.barrier == removed.barrier, "nested broker barrier");
        owner.output->fulfil_reply(removed.barrier,
            std::make_shared<const std::string>("+EXEC\r\n"));
        require(owner.output->wait(removed.barrier) && !owner.wire.empty() &&
            owner.wire[0] == "+EXEC\r\n",
            "self publication overtook outer reply");
        owner.flush();
        require(owner.wire.size() == 2 && owner.wire[1].find("self") !=
            std::string::npos, "self publication vanished after unsubscribe");
    };
    acl::fiber::schedule();
}
static void acl_rules(acl_policy& policy,
    const std::vector<std::string>& rules) {
    const auto error = policy.set("listener", borrowed(rules));
    if (!error.empty()) std::cerr << error << '\n';
    require(error.empty(), "ACL test rules");
}
static void permissions() {
    pubsub_broker broker;
    acl_policy policy;
    acl_rules(policy, {"on", "nopass", "resetchannels", "&a*", "-@all"});
    client owner(broker, 1, 2, policy.find("listener"));
    owner.change(kind::channel, {"abc"}, true, false);
    owner.change(kind::pattern, {"a*"}, true, false);
    owner.change(kind::shard, {"a-shard"}, true, false);
    broker.validate_permissions();
    require(!owner.denied, "existing channels require command permission");
    acl_rules(policy, {"resetchannels", "&abc", "(&a*)"});
    broker.validate_permissions();
    require(!owner.denied, "existing channels did not use selector union");
    acl_rules(policy, {"clearselectors", "resetchannels", "&abc", "&a-shard"});
    broker.validate_permissions();
    require(owner.denied == 1, "pattern authorization used glob containment");
    broker.retire(owner.endpoint);
    require(!owner.endpoint->counts().total(), "permission close retirement");
}
static void queries_and_quota() {
    pubsub_broker first, second;
    client owner(first, 11), foreign(second, 11);
    owner.change(kind::channel, {"b", std::string("a\0x", 3)}, true, false);
    const std::string pattern("a*");
    require(first.channels(kind::channel, &pattern) ==
        std::vector<std::string>({std::string("a\0x", 3)}),
        "binary channel glob enumeration");
    require(first.subscribers(kind::channel, borrowed({"b", "missing", "b"}))
        == std::vector<size_t>({1, 0, 1}), "NUMSUB duplicate request order");
    first.retire(foreign.endpoint);
    require(first.counts().channel == 2 && !foreign.endpoint->counts().total(),
        "foreign endpoint corrupted broker");
    client bounded(first, 12, 2, nullptr, 4096);
    bounded.change(kind::channel, {"b"}, true, false);
    require(first.publish(kind::channel, "b", std::string(8192, 'x')) == 2 &&
        bounded.closed == 1 && bounded.output->closed(),
        "slow subscriber quota or matched receiver count");
    first.retire(bounded.endpoint); first.retire(owner.endpoint);
    require(first.channels(kind::channel).empty(), "retire kept channel names");
}
static void faults() {
    unsigned failed = 0, succeeded = 0;
    const std::vector<std::string> names{std::string(80, 'x'),
        std::string(80, 'y'), std::string(80, 'x'), "present"};
    const auto args = borrowed(names);
    for (auto type : {kind::channel, kind::pattern, kind::shard})
        for (bool existing : {false, true})
            for (bool add : {false, true})
                for (long allowance = 0; allowance < 96; ++allowance) {
                    pubsub_broker broker;
                    client owner(broker, 1), other(broker, 2);
                    owner.change(type, {"present", "retained"}, true, false);
                    if (!add) owner.change(type, names, true, false);
                    if (existing) other.change(type, names, true, false);
                    const auto before = owner.endpoint->counts().total();
                    const auto owned = owner.endpoint->owned();
                    const auto channels = broker.channels(type);
                    bool success = false;
                    try {
                        budget = allowance;
                        const auto reply = broker.change(owner.endpoint,
                            type, args, add);
                        budget = -1; success = reply.barrier != 0;
                    } catch (const std::bad_alloc&) { budget = -1; }
                    if (success) {
                        ++succeeded;
                        require(owner.endpoint->counts().total() ==
                            (add ? 4U : 1U), "successful fault-path count");
                        const auto counts = broker.subscribers(type, args);
                        for (const auto current : counts)
                            require(current == (existing ? 1U : 0U) +
                                (add ? 1U : 0U), "committed route count");
                    } else {
                        ++failed;
                        require(owner.endpoint->counts().total() == before &&
                            owner.endpoint->owned() == owned &&
                            broker.channels(type) == channels,
                            "subscription failure exposed partial indexes");
                    }
                    budget = 0;
                    broker.retire(owner.endpoint);
                    budget = -1;
                    require(!owner.endpoint->counts().total() &&
                        !owner.endpoint->owned(), "retire allocated or leaked");
                }
    require(failed && succeeded && failed + succeeded == 1152,
        "broker fault budget coverage");
    std::cout << "PubSub membership budgets: failed=" << failed <<
        " succeeded=" << succeeded << '\n';
}
static void concurrent() {
    go[] {
        pubsub_broker broker;
        client first(broker, 1), second(broker, 2, 3);
        first.output->start(); second.output->start();
        first.change(kind::channel, {"a"});
        second.change(kind::channel, {"a"});
        first.wire.clear(); second.wire.clear();
        std::atomic<unsigned> done{0};
        std::vector<std::thread> producers;
        for (unsigned t = 0; t < 4; ++t) producers.emplace_back([&, t] {
            for (unsigned i = 0; i < 1000; ++i)
                require(broker.publish(kind::channel, "a", std::to_string(t)
                    + ":" + std::to_string(i)) == 2, "parallel publish count");
            ++done;
        });
        while (done.load() != 4) acl::fiber::delay(1);
        for (auto& producer : producers) producer.join();
        first.flush(); second.flush();
        require(first.wire.size() == 4000 && second.wire.size() == 4000,
            "parallel fanout lost deliveries");
        unsigned next[4] = {};
        for (size_t i = 0; i < first.wire.size(); ++i) {
            const auto& wire = first.wire[i];
            require(wire.substr(1) == second.wire[i].substr(1),
                "subscribers observed different publication order");
            const auto position = wire.rfind("\r\n", wire.size() - 3);
            const auto value = wire.substr(position + 2,
                wire.size() - position - 4);
            const auto t = static_cast<unsigned>(value[0] - '0');
            require(t < 4 && value == std::to_string(t) + ":" +
                std::to_string(next[t]++), "publisher FIFO violated");
        }
    };
    acl::fiber::schedule();
}
static void shared_fanout() {
    go[] {
        pubsub_broker broker;
        std::vector<std::shared_ptr<connection_output>> outputs;
        std::vector<pubsub_broker::subscriber> clients;
        const size_t recipients = 128;
        std::vector<uintptr_t> pointers(recipients);
        const std::string payload(1024 * 1024, 'x'), channel("fanout");
        for (size_t i = 0; i < recipients; ++i) {
            auto output = std::make_shared<connection_output>(
                [&, i](const char* data, size_t size) {
                    if (size > payload.size())
                        pointers[i] = reinterpret_cast<uintptr_t>(data);
                    return size;
                }, [](size_t, bool) {}, [] {});
            auto endpoint = broker.attach(i + 1, output, i % 2 ? 3 : 2,
                nullptr, [](bool) {});
            output->start();
            const auto reply = broker.change(endpoint, kind::channel,
                {&channel}, true);
            output->fulfil_reply(reply.barrier,
                std::make_shared<const std::string>(reply.acknowledgements));
            require(output->wait(reply.barrier), "fanout subscribe");
            outputs.push_back(std::move(output));
            clients.push_back(std::move(endpoint));
        }
        require(broker.publish(kind::channel, channel, payload) == recipients,
            "fanout delivery count");
        for (const auto& output : outputs) {
            const auto id = output->reserve_reply();
            output->fulfil_reply(id, std::make_shared<const std::string>());
            require(output->wait(id), "fanout flush");
        }
        for (size_t i = 0; i < recipients; ++i)
            require(pointers[i] && pointers[i] == pointers[i % 2],
                "payload serialized or copied for every recipient");
        require(pointers[0] != pointers[1], "protocol frame cache alias");
        for (const auto& endpoint : clients) broker.retire(endpoint);
        for (const auto& output : outputs) output->stop();
        require(!broker.counts().total(), "fanout retirement");
        const auto encoded = pubsub_message(channel, payload, nullptr,
            false, 3);
        require(encoded.capacity() < encoded.size() * 2,
            "frame retained a doubled payload buffer");
        std::cout << "PubSub 128-recipient fanout shared two protocol frames\n";
    };
    acl::fiber::schedule();
}
int main() {
    acl::acl_cpp_init();
    encoding(); membership(); protocol_order(); permissions();
    queries_and_quota(); faults(); concurrent(); shared_fanout();
    std::cout << "PubSub routes, frames, ordering, ACL and rollback passed\n";
}
