#pragma once
#include <string>
#include <vector>
namespace pkv {
struct list_block_plan {
    bool sorted = false;
    bool move = false, nested = false, left = true, to_left = true;
    long long count = 1, timeout_ms = 0;
    std::vector<std::string> keys;
    std::string destination, error;
    bool parse(const char* command,
        const std::vector<const std::string*>& arguments);
private:
    bool timeout(const std::string& value);
    bool multiple(const std::vector<const std::string*>& arguments);
};
long long blocking_clock_ms();
} // namespace pkv
