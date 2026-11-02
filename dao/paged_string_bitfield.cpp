#include "stdafx.h"
#include "paged_string.h"
#include "string_bitop_source.h"

namespace pkv {
namespace dao {
namespace {
size_t field_bytes(const bitfield_operation& operation) {
    return static_cast<size_t>((operation.offset % 8 + operation.bits + 7) / 8);
}
bool execute_field(string_store& store, const bitfield_operation& operation,
    bitfield_reply& reply) {
    const auto byte = static_cast<long long>(operation.offset / 8);
    const auto count = field_bytes(operation);
    std::string bytes;
    if (!store.range(byte, byte + count - 1, bytes)) return false;
    bytes.resize(count, '\0');
    const auto skip = static_cast<unsigned>(operation.offset % 8);
    const auto old = field_read(bytes, skip, operation.bits, operation.sign);
    if (operation.command == field_command::get) {
        reply.value = old; return true;
    }
    long long value;
    if (!field_update(operation, old, value)) {
        reply.null = true; return true;
    }
    reply.value = operation.command == field_command::set ? old : value;
    if (old == value) return true;
    field_write(bytes, skip, operation.bits, static_cast<uint64_t>(value));
    return store.write_field(static_cast<uint64_t>(byte), bytes);
}
} // namespace

bool paged_string::bitfield(shared_db& db, const std::string& key,
    const std::vector<bitfield_operation>& operations, bool readonly,
    std::vector<bitfield_reply>& out) {
    reset(); out.clear();
    uint64_t length = 0;
    for (const auto& operation : operations) {
        if (!valid_bitfield(operation)) {
            read_status_ = read_status::invalid_record; return false;
        }
        if (operation.command != field_command::get)
            length = std::max(length,
                (operation.offset + operation.bits + 7) / 8);
    }
    if (readonly && length) {
        read_status_ = read_status::readonly_bitfield; return false;
    }
    if (!length) {
        const auto status = db->transact_page_reads({key},
            [&](page_transaction& tx) {
            string_bitop_source source(db, tx, key, unix_time_ms());
            if (!source.open()) {
                read_status_ = source.error(); return false;
            }
            out.reserve(operations.size());
            for (const auto& operation : operations) {
                std::string bytes;
                const auto count = field_bytes(operation);
                if (!source.read(operation.offset / 8, count, bytes)) {
                    read_status_ = source.error(); return false;
                }
                bytes.resize(count, '\0');
                bitfield_reply reply;
                reply.value = field_read(bytes,
                    static_cast<unsigned>(operation.offset % 8),
                    operation.bits, operation.sign);
                out.push_back(reply);
            }
            return true;
        });
        if (result(status)) return true;
        out.clear(); return false;
    }
    const bool ok = run(db, key, [&](string_store& store) {
        if (length > store.metadata().length &&
            !store.write_field(length - 1, std::string(1, '\0'))) return false;
        out.reserve(operations.size());
        for (const auto& operation : operations) {
            bitfield_reply reply;
            if (!execute_field(store, operation, reply)) return false;
            out.push_back(reply);
        }
        return true;
    });
    if (!ok) out.clear();
    return ok;
}

} // namespace dao
} // namespace pkv
