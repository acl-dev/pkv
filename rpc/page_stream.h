#pragma once
#include "page_batch.h"

namespace pkv {

// Codec for negotiated PKV2 transaction frames. Caller owns connection closure
// on failure and must prevent other writers from interleaving this whole batch.
class page_stream {
public:
    static bool send(acl::shared_stream& connection, const page_batch& batch,
        unsigned protocol = 2);
    static bool read(acl::shared_stream& connection, page_frame& out);

    // Dispatchers that already consumed a 20-byte header can continue here.
    // Invalid/truncated frames and allocation failures leave out unchanged.
    static bool read_payload(acl::shared_stream& connection,
        const char* header, page_frame& out);
};

} // namespace pkv
