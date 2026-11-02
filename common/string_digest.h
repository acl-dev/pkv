#pragma once
#include <string>

namespace pkv {
// Streaming XXH3-64, using xxHash's aligned state allocation in C++11 too.
class string_digest {
public:
    string_digest();
    ~string_digest();
    string_digest(const string_digest&) = delete;
    string_digest& operator=(const string_digest&) = delete;
    void update(const std::string& bytes);
    std::string finish() const;
    static std::string value(const std::string& bytes);
    static bool equal(const std::string& expected, const std::string& actual);
private:
    void* state_ = nullptr;
};
} // namespace pkv
