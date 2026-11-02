#include "move_intent.h"
#include "page_wire.h"
#include <new>

namespace pkv {
bool move_intent::measure(const family_move& move, size_t& bytes) {
    if (move.source == move.target ||
        move.source >= page_batch::max_databases ||
        move.target >= page_batch::max_databases ||
        move.key.size() > page_batch::max_key_bytes ||
        move.source_root.size() > page_transaction::max_value_bytes ||
        move.target_root.size() > page_transaction::max_value_bytes ||
        (!move.target_found && !move.target_root.empty())) return false;
    bytes = 36 + move.key.size() + move.source_root.size() +
        move.target_root.size();
    return true;
}
bool move_intent::decode(const page_frame& frame, family_move& out) {
    if (frame.protocol != 4 ||
        frame.operation != page_operation::family_move || frame.id ||
        frame.value.size() < 16) return false;
    const auto* data = frame.value.data();
    const auto flags = page_wire::get32(data + 4);
    const auto from = page_wire::get32(data + 8);
    const auto to = page_wire::get32(data + 12);
    if (flags > 1 || from > page_transaction::max_value_bytes ||
        to > page_transaction::max_value_bytes ||
        uint64_t(16) + from + to != frame.value.size()) return false;
    try {
        family_move move;
        move.source = frame.database; move.target = page_wire::get32(data);
        move.key = frame.key; move.target_found = flags != 0;
        move.source_root.assign(data + 16, from);
        move.target_root.assign(data + 16 + from, to);
        size_t bytes;
        if (!measure(move, bytes)) return false;
        out = std::move(move); return true;
    } catch (const std::bad_alloc&) { return false; }
}
bool move_intent::send(acl::shared_stream& connection,
    const family_move& move) {
    size_t bytes;
    if (!connection || !connection->opened() || !measure(move, bytes))
        return false;
    char header[20], payload[16];
    page_wire::put32(header, page_wire::ordered_magic);
    page_wire::put32(header + 4,
        static_cast<uint32_t>(page_operation::family_move));
    page_wire::put32(header + 8, move.source);
    page_wire::put32(header + 12, static_cast<uint32_t>(move.key.size()));
    page_wire::put32(header + 16, static_cast<uint32_t>(bytes - 20 -
        move.key.size()));
    page_wire::put32(payload, move.target);
    page_wire::put32(payload + 4, move.target_found ? 1 : 0);
    page_wire::put32(payload + 8,
        static_cast<uint32_t>(move.source_root.size()));
    page_wire::put32(payload + 12,
        static_cast<uint32_t>(move.target_root.size()));
    iovec vectors[5] = {
        {header, sizeof(header)},
        {const_cast<char*>(move.key.data()), move.key.size()},
        {payload, sizeof(payload)},
        {const_cast<char*>(move.source_root.data()), move.source_root.size()},
        {const_cast<char*>(move.target_root.data()), move.target_root.size()}
    };
    return connection->writev(vectors, 5) == static_cast<int>(bytes);
}
} // namespace pkv
