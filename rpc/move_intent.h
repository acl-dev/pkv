#pragma once
#include "page_batch.h"
#include "db/family_move.h"
#include "acl-lib/acl_cpp/lib_acl.hpp"

namespace pkv {
// One PKV4 record: target, flags, source length, target length (four BE32s),
// then the two raw roots. Source DB and binary key use the ordinary header.
// No collection payload is encoded here.
class move_intent {
public:
    static bool measure(const family_move& move, size_t& bytes);
    static size_t expanded_bytes(const family_move& move) {
        return sizeof(family_move) + move.key.size() +
            move.source_root.size() + move.target_root.size() + 96;
    }
    static bool decode(const page_frame& frame, family_move& out);
    static bool send(acl::shared_stream& connection, const family_move& move);
};
} // namespace pkv
