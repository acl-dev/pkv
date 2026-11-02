#include "stdafx.h"
#include "common/key_wait.h"
#include "common/execution_gate.h"
#include <cstdlib>
#include <iostream>
using namespace pkv;
int main() {
    acl::acl_cpp_init();
    auto queue = std::make_shared<key_wait_registry>();
    auto token = queue->subscribe({"key"});
    bool waiting = false, finished = false;
    auto victim = acl::go_fiber() > [&] {
        execution_guard one, two;
        key_wait_event event;
        {
            execution_pause pause;
            waiting = true;
            if (token->wait(-1, event)) std::abort();
        }
        token.reset(); finished = true;
    };
    go[&] {
        while (!waiting) acl::fiber::yield();
        execution_guard writer(true);
        victim->kill(false);
        for (unsigned i = 0; i < 20; ++i) acl::fiber::yield();
        if (finished) std::abort();
    };
    acl::fiber::schedule();
    if (!finished || queue->subscribers()) std::abort();
    std::cout << "Killed waiter resumed only after exclusive actor left\n";
}
