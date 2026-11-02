#pragma once
#include <atomic>
#include <functional>
#include <memory>
namespace acl { class socket_stream; }
namespace pkv {
// Promoted connections keep one reader fiber. It owns socket input, queues
// bounded raw chunks, and never touches the command parser or its object cache.
class connection_input {
public:
    using receive_callback = std::function<void(size_t)>;
    connection_input(acl::socket_stream& socket, receive_callback received,
        std::function<void()> closed, size_t limit = 1024ULL * 1024 * 1024);
    ~connection_input();
    void start();
    void stop();
    int read(char* data, size_t capacity);
    size_t pending() const;
    size_t owned() const;
    bool closed() const;
private:
    struct state;
    std::unique_ptr<state> state_;
};
} // namespace pkv
