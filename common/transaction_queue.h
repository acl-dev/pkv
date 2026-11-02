#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace pkv {
class redis_object;

class transaction_queue {
public:
    enum class result { queued, full };
    explicit transaction_queue(size_t bytes = 64U * 1024 * 1024,
        size_t commands = 65536) : max_bytes_(bytes), max_commands_(commands) {}
    const std::vector<std::vector<std::string>>& commands() const {
        return commands_;
    }
    bool active() const { return active_; }
    bool failed() const { return failed_; }
    bool start();
    void fail() { if (active_) failed_ = true; }
    void clear();
    result append(const redis_object& object);
    std::vector<std::vector<std::string>> take();
private:
    size_t max_bytes_, max_commands_, bytes_ = 0;
    bool active_ = false, failed_ = false;
    std::vector<std::vector<std::string>> commands_;
};
} // namespace pkv
