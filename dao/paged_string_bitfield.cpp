#include "stdafx.h"
#include "paged_string.h"
#include "string_bitop_source.h"

namespace pkv {
namespace dao {
namespace {
size_t field_bytes(const bitfield_operation& operation) {
    return static_cast<size_t>((operation.offset % 8 + operation.bits + 7) / 8);
}
bool edit_field(char* bytes, const bitfield_operation& operation, bitfield_reply& reply) {
    const auto skip = static_cast<unsigned>(operation.offset % 8);
    const auto old = field_read(bytes, skip, operation.bits, operation.sign);
    if (operation.command == field_command::get) {
        reply.value = old; return false;
    }
    long long value;
    if (!field_update(operation, old, value)) {
        reply.null = true; return false;
    }
    reply.value = operation.command == field_command::set ? old : value;
    if (old == value) return false;
    field_write(bytes, skip, operation.bits, static_cast<uint64_t>(value));
    return true;
}
bool execute_field(string_store& store, const bitfield_operation& operation,
    bitfield_reply& reply) {
    const auto byte = static_cast<long long>(operation.offset / 8);
    const auto count = field_bytes(operation);
    std::string bytes;
    if (!store.range(byte, byte + count - 1, bytes)) return false;
    bytes.resize(count, '\0');
    return !edit_field(&bytes[0], operation, reply) ||
        store.write_field(static_cast<uint64_t>(byte), bytes);
}

db::inline_status edit_field_window(db& database, const std::string& key,
    const std::vector<bitfield_operation>& operations, std::vector<bitfield_reply>& replies) {
    uint64_t first = operations.front().offset / 8, end = first;
    for (const auto& op : operations) {
        first = std::min(first, op.offset / 8);
        end = std::max(end, (op.offset + op.bits + 7) / 8);
    }
    struct field_edit {
        const std::vector<bitfield_operation>& operations;
        std::vector<bitfield_reply>& replies;
        uint64_t first;
    } request{operations, replies, first};
    const auto apply = [](void* raw, char* bytes) {
        auto& edit = *static_cast<field_edit*>(raw);
        edit.replies.resize(edit.operations.size());
        bool changed = false;
        for (size_t i = 0; i < edit.operations.size(); ++i) {
            const auto& op = edit.operations[i];
            if (edit_field(bytes + (op.offset / 8 - edit.first), op, edit.replies[i]))
                changed = true;
        }
        return changed;
    };
    return database.edit_string_bytes(key, first, static_cast<size_t>(end - first),
        apply, &request);
}
} // namespace

bool paged_string::bitfield(shared_db& db, const std::string& key,
    const std::vector<bitfield_operation>& operations, bool readonly,
    std::vector<bitfield_reply>& out) {
    reset(); out.clear();
    uint64_t length = 0;
    for (const auto& field : operations) {
        if (!valid_bitfield(field)) {
            read_status_ = read_status::invalid_record; return false;
        }
        if (field.command != field_command::get)
            length = std::max(length,
                (field.offset + field.bits + 7) / 8);
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
            for (const auto& field : operations) {
                std::string bytes;
                const auto count = field_bytes(field);
                if (!source.read(field.offset / 8, count, bytes)) {
                    read_status_ = source.error(); return false;
                }
                bytes.resize(count, '\0');
                bitfield_reply reply;
                reply.value = field_read(bytes,
                    static_cast<unsigned>(field.offset % 8),
                    field.bits, field.sign);
                out.push_back(reply);
            }
            return true;
        });
        if (result(status)) return true;
        out.clear(); return false;
    }
    // Keep the work performed under a native shard lock bounded. Any range
    // requiring growth or zero-padding is handled by the page transaction.
    if (!scope_ && operations.size() <= 8) {
        const auto direct = edit_field_window(*db, key, operations, out);
        if (direct == db::inline_status::found) {
            read_status_ = read_status::ok;
            return true;
        }
        if (direct == db::inline_status::error) {
            out.clear(); read_status_ = read_status::storage_error; return false;
        }
    }
    const bool ok = run(db, key, [&](string_store& store) {
        if (length > store.metadata().length &&
            !store.write_field(length - 1, std::string(1, '\0'))) return false;
        out.reserve(operations.size());
        for (const auto& field : operations) {
            bitfield_reply reply;
            if (!execute_field(store, field, reply)) return false;
            out.push_back(reply);
        }
        return true;
    });
    if (!ok) out.clear();
    return ok;
}

} // namespace dao
} // namespace pkv
