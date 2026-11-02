#include "stdafx.h"
#include "exec_stream.h"
#include "page_stream.h"
#include "page_wire.h"
#include "move_intent.h"

namespace pkv {
namespace {
bool control(acl::shared_stream& connection, page_operation operation,
    unsigned protocol, const char* payload, size_t length) {
    char header[20] = {};
    page_wire::put32(header, page_wire::magic_for(protocol));
    page_wire::put32(header + 4, static_cast<uint32_t>(operation));
    page_wire::put32(header + 16, static_cast<uint32_t>(length));
    iovec vectors[2];
    vectors[0].iov_base = header; vectors[0].iov_len = sizeof(header);
    vectors[1].iov_base = const_cast<char*>(payload);
    vectors[1].iov_len = length;
    return connection->writev(vectors, length ? 2 : 1) ==
        static_cast<int>(sizeof(header) + length);
}
} // namespace
bool exec_stream::send(acl::shared_stream& connection,
    const exec_batch& batch, unsigned protocol) {
    if ((protocol != 3 && protocol != 4) || batch.protocol() > protocol)
        return false;
    char begin[16];
    page_wire::put32(begin, static_cast<uint32_t>(batch.protocol() == 4 ?
        batch.entries().size() : batch.batches().size()));
    page_wire::put32(begin + 4, static_cast<uint32_t>(batch.records()));
    page_wire::put64(begin + 8, batch.bytes());
    if (!control(connection, page_operation::exec_begin, protocol, begin,
        sizeof(begin)))
        return false;
    if (batch.protocol() == 4) {
        for (const auto& entry : batch.entries()) {
            const bool sent = entry.pages() ?
                page_stream::send(connection, *entry.pages(), protocol) :
                move_intent::send(connection, *entry.move());
            if (!sent) return false;
        }
    } else for (const auto& pages : batch.batches())
        if (!page_stream::send(connection, *pages, protocol)) return false;
    return control(connection, page_operation::exec_commit, protocol,
        nullptr, 0);
}
} // namespace pkv
