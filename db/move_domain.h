#pragma once
#include "move_journal.h"
#include <memory>
#include <string>
#include <vector>

namespace pkv {
// Startup-only control ownership. Prepare before native DB open; bind only
// after DB 0 owns all native stores, before recovery or any traffic/expiry.
class move_domain {
public:
    explicit move_domain(move_journal_io* operations = nullptr);
    ~move_domain();
    move_domain(const move_domain&) = delete;
    move_domain& operator=(const move_domain&) = delete;
    std::string prepare(const std::string& backend, const std::string& paths,
        size_t databases, const std::string& journal_path);
    std::string bind();
    // The bound journal and its control locks must outlive all DB users.
    move_journal* journal();
    bool enabled() const;
    const std::vector<std::string>& roots() const;
    const std::string& path() const;
    const std::string& identity() const;
private:
    struct implementation;
    std::unique_ptr<implementation> data_;
};
} // namespace pkv
