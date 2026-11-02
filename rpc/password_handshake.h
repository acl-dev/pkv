#pragma once
#include "acl-lib/acl_cpp/lib_acl.hpp"
#include "common/password_policy.h"

namespace pkv {
// These run before a publisher registers a subscriber or emits any records.
// Protected streams never retry as unauthenticated legacy streams.
bool accept_password(const acl::shared_stream& conn,
    const password_policy& policy);
bool send_password(const acl::shared_stream& conn,
    const std::string& password);
} // namespace pkv
