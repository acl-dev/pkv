#pragma once
#include "map_page.h"

namespace pkv {

struct zset_root {
    int64_t expires_at_ms = -1;
    uint64_t elements = 0;
    uint64_t members = 0;
    uint64_t scores = 0;
    uint64_t last_id = 0;
};

class zset_page_codec {
public:
    static const size_t root_bytes = 64;
    static map_page_status encode_root(const zset_root& root,
        std::string& out);
    static map_page_status decode_root(const std::string& bytes,
        zset_root& out);
    // Big-endian IEEE-754 transform; lexical byte order equals numeric order.
    // Both zeros have the same key. NaN and noncanonical negative zero fail.
    static bool encode_score(double score, std::string& out);
    static bool decode_score(const std::string& bytes, double& out);
    // Member payloads preserve signed zero; sorting keys normalize it.
    static bool encode_value(double score, std::string& out);
    static bool decode_value(const std::string& bytes, double& out);
    static bool score_key(double score, const std::string& member,
        std::string& out);
};

} // namespace pkv
