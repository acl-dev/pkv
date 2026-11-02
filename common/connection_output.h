#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace pkv {
// One owner fiber reserves/fulfils replies. Any thread can append pushes.
// The writer is the only sender; producers never wait for socket progress.
// send may suspend, but only on the owning worker. sent/closed must not throw.
// closed must interrupt any active socket write and must not call stop().
// start, stop and wait belong to the owner; stop joins before callbacks die.
class connection_output {
public:
    using frame = std::shared_ptr<const std::string>;
    using ticket = uint64_t;
    using send_callback = std::function<size_t(const char*, size_t)>;
    using sent_callback = std::function<void(size_t, bool)>;
    connection_output(send_callback send, sent_callback sent,
        std::function<void()> closed, size_t limit = 32ULL * 1024 * 1024);
    ~connection_output();
    void start();
    void stop();
    ticket reserve_reply();
    // Call with the subscription broker locked, before changing membership.
    // Earlier external pushes move ahead of this reply. Self pushes stay after.
    bool capture_external();
    bool fulfil_reply(ticket reply, frame data);
    bool append(frame data, bool self = false);
    bool wait(ticket reply);
    bool limit(size_t bound);
    bool closed() const;
    size_t pending() const;
    size_t owned() const;
    size_t frames() const;
private:
    struct state;
    std::unique_ptr<state> state_;
};
} // namespace pkv
