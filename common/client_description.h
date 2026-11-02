#pragma once
#include "client_registry.h"

namespace pkv {
bool append_client_description(std::string& out,
    const client_registry::connection& entry, long long now, int type = -1);
} // namespace pkv
