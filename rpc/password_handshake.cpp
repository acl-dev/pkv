#include "password_handshake.h"
#include "common/redis_integer.h"
#include <algorithm>
#include <chrono>

namespace pkv {
namespace {
using clock = std::chrono::steady_clock;
bool arm(const acl::shared_stream& conn, clock::time_point deadline) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - clock::now()).count();
    if (left <= 0) return false;
    conn->set_rw_timeout(static_cast<int>((left + 999) / 1000));
    return true;
}
bool read_until(const acl::shared_stream& conn, char* bytes, size_t size,
    clock::time_point deadline) {
    size_t offset = 0;
    while (offset < size) {
        if (!arm(conn, deadline)) return false;
        const int count = conn->read(bytes + offset, size - offset, false);
        if (count <= 0 || static_cast<size_t>(count) > size - offset)
            return false;
        offset += static_cast<size_t>(count);
    }
    return true;
}
bool read_header(const acl::shared_stream& conn, std::string& line,
    clock::time_point deadline) {
    while (line.size() < 64) {
        char ch;
        if (!read_until(conn, &ch, 1, deadline)) return false;
        line += ch;
        if (ch == '\n') {
            if (line.size() < 2 || line[line.size() - 2] != '\r') return false;
            line.resize(line.size() - 2);
            return true;
        }
    }
    return false;
}
}
bool accept_password(const acl::shared_stream& conn,
    const password_policy& policy) {
    if (!policy.protected_server()) return true;
    const auto deadline = clock::now() + std::chrono::seconds(5);
    std::string line;
    if (!read_header(conn, line, deadline) || line.size() < 10 ||
        line.compare(0, 9, "PKV-AUTH ") != 0) return false;
    long long length;
    if (!parse_redis_integer(line.substr(9), length) || length < 0 ||
        static_cast<unsigned long long>(length) != policy.password_size())
        return false;
    char bytes[1024];
    bool matched = true;
    size_t offset = 0;
    while (offset < policy.password_size()) {
        const size_t count = std::min(sizeof(bytes),
            policy.password_size() - offset);
        if (!read_until(conn, bytes, count, deadline)) return false;
        const bool fragment = policy.matches_fragment(offset, bytes, count);
        matched = fragment && matched;
        offset += count;
    }
    char ending[2];
    if (!read_until(conn, ending, 2, deadline) || ending[0] != '\r' ||
        ending[1] != '\n' || !matched || !arm(conn, deadline)) return false;
    if (conn->write("+OK\r\n", 5) != 5) return false;
    conn->set_rw_timeout(-1);
    return true;
}
bool send_password(const acl::shared_stream& conn,
    const std::string& password) {
    if (password.empty()) return true;
    const auto deadline = clock::now() + std::chrono::seconds(5);
    const auto header = "PKV-AUTH " + std::to_string(password.size()) + "\r\n";
    if (!arm(conn, deadline) || conn->write(header.data(), header.size()) !=
        static_cast<int>(header.size())) return false;
    size_t offset = 0;
    while (offset < password.size()) {
        const auto count = std::min(size_t(1024), password.size() - offset);
        if (!arm(conn, deadline) ||
            conn->write(password.data() + offset, count) !=
                static_cast<int>(count)) return false;
        offset += count;
    }
    if (!arm(conn, deadline) || conn->write("\r\n", 2) != 2) return false;
    char reply[5];
    if (!read_until(conn, reply, sizeof(reply), deadline) ||
        std::string(reply, sizeof(reply)) != "+OK\r\n") return false;
    conn->set_rw_timeout(-1);
    return true;
}
} // namespace pkv
