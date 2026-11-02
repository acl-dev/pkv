#pragma once
#include <string>

namespace pkv {
// Authoritative same-key MOVE checked against the staged raw prefix roots.
// Presence is separate from bytes: a found empty root is not an absent root.
struct family_move {
    unsigned source = 0, target = 0;
    std::string key, source_root;
    bool target_found = false;
    std::string target_root;
};
} // namespace pkv
