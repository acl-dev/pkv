#pragma once
#include <cstddef>
#include <string>

namespace pkv {
struct acl_file_result {
    std::string error;
    bool replaced = false;
    bool ok() const { return error.empty(); }
};
// Injectable syscall boundary; the production implementation uses POSIX IO.
class acl_file_io {
public:
    virtual ~acl_file_io() = default;
    virtual int temporary(char* path);
    virtual int directory(const char* path);
    virtual long long write(int fd, const char* data, size_t size);
    virtual int sync(int fd);
    virtual int close(int fd);
    virtual int replace(const char* from, const char* to);
    virtual int remove(const char* path);
};
std::string acl_read_file(const std::string& path, std::string& text);
acl_file_result acl_save_file(const std::string& path, const std::string& text,
    acl_file_io* operations = nullptr);
} // namespace pkv
