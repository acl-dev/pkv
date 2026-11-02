#pragma once
#include <string>
#include <vector>

namespace pkv {
class redis_handler;
class transaction_workspace {
public:
    transaction_workspace(redis_handler& handler,
        const std::vector<std::vector<std::string>>& commands);
    ~transaction_workspace();
    bool commit();
    transaction_workspace(const transaction_workspace&) = delete;
    transaction_workspace& operator=(const transaction_workspace&) = delete;
private:
    redis_handler& handler_;
};
} // namespace pkv
