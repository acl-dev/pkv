#include "stdafx.h"
#include "page_stream.h"
#include "page_wire.h"
#include <new>

namespace pkv {
namespace {

bool write_frame(acl::shared_stream& connection, page_operation operation,
    unsigned protocol, unsigned database, const std::string& key,
    const char* value,
    size_t length, uint64_t id = 0) {
    const bool page = operation == page_operation::page_write ||
        operation == page_operation::page_erase;
    char header[20], number[8];
    page_wire::put32(header, page_wire::magic_for(protocol));
    page_wire::put32(header + 4, static_cast<uint32_t>(operation));
    page_wire::put32(header + 8, database);
    page_wire::put32(header + 12, static_cast<uint32_t>(key.size()));
    page_wire::put32(header + 16,
        static_cast<uint32_t>(length + (page ? 8 : 0)));
    iovec vectors[4];
    vectors[0].iov_base = header;
    vectors[0].iov_len = sizeof(header);
    int count = 1;
    if (!key.empty()) {
        vectors[count].iov_base = const_cast<char*>(key.data());
        vectors[count++].iov_len = key.size();
    }
    if (page) {
        page_wire::put64(number, id);
        vectors[count].iov_base = number;
        vectors[count++].iov_len = sizeof(number);
    }
    if (length) {
        vectors[count].iov_base = const_cast<char*>(value);
        vectors[count++].iov_len = length;
    }
    const auto bytes = sizeof(header) + key.size() + length + (page ? 8 : 0);
    return connection->writev(vectors, count) == static_cast<int>(bytes);
}

bool valid_lengths(page_operation operation, size_t key, size_t value,
    unsigned protocol) {
    if (key > page_batch::max_key_bytes) {
        return false;
    }
    switch (operation) {
    case page_operation::exec_begin:
        return (protocol == 3 || protocol == 4) && key == 0 && value == 16;
    case page_operation::exec_commit:
        return (protocol == 3 || protocol == 4) && key == 0 && value == 0;
    case page_operation::family_move:
        return protocol == 4 && value >= 16 &&
            value <= 16 + 2 * page_transaction::max_value_bytes;
    case page_operation::begin: return key == 0 && value == 16;
    case page_operation::commit: return key == 0 && value == 0;
    case page_operation::root_keep:
    case page_operation::root_erase: return value == 0;
    case page_operation::root_maintenance:
    case page_operation::root_replace:
    case page_operation::root_write:
        return value <= page_transaction::max_value_bytes;
    case page_operation::page_write:
        return value >= 8 && value <= page_transaction::max_value_bytes + 8;
    case page_operation::page_erase: return value == 8;
    }
    return false;
}

bool read_bytes(acl::shared_stream& connection, size_t size, std::string& out) {
    std::string bytes(size, '\0');
    if (size && connection->read(&bytes[0], size, true) !=
        static_cast<int>(size)) {
        return false;
    }
    out.swap(bytes);
    return true;
}

} // namespace

bool page_stream::send(acl::shared_stream& connection,
    const page_batch& batch, unsigned protocol) {
    if (protocol < 2 || protocol > 4) return false;
    char begin[16];
    page_wire::put32(begin, static_cast<uint32_t>(batch.size().roots));
    page_wire::put32(begin + 4, static_cast<uint32_t>(batch.size().pages));
    page_wire::put64(begin + 8, batch.size().bytes);
    const std::string empty;
    const auto database = batch.database();
    if (!write_frame(connection, page_operation::begin, protocol, database,
        empty, begin, sizeof(begin))) {
        return false;
    }
    for (const auto& root : batch.roots()) {
        const auto operation = root.action == db::update_action::write ?
            (root.maintenance_only ? page_operation::root_maintenance :
                root.clear_pages ? page_operation::root_replace :
                page_operation::root_write) :
                root.action == db::update_action::erase ?
                    page_operation::root_erase : page_operation::root_keep;
        if (!write_frame(connection, operation, protocol, database, root.key,
            root.value.data(), root.action == db::update_action::write ?
                root.value.size() : 0)) {
            return false;
        }
    }
    for (const auto& page : batch.pages()) {
        if (!write_frame(connection, page.action == db::update_action::write ?
            page_operation::page_write : page_operation::page_erase, protocol,
            database, page.owner, page.value.data(), page.value.size(),
            page.id)) {
            return false;
        }
    }
    return write_frame(connection, page_operation::commit, protocol,
        database, empty, nullptr, 0);
}

bool page_stream::read(acl::shared_stream& connection, page_frame& out) {
    char header[20];
    if (connection->read(header, sizeof(header), true) != sizeof(header)) {
        return false;
    }
    return read_payload(connection, header, out);
}

bool page_stream::read_payload(acl::shared_stream& connection,
    const char* header, page_frame& out) {
    if (!header) return false;
    const auto magic = page_wire::get32(header);
    const unsigned protocol = page_wire::version(magic);
    if (!protocol) return false;
    const auto operation = static_cast<page_operation>(
        page_wire::get32(header + 4));
    const auto database = page_wire::get32(header + 8);
    const auto key = page_wire::get32(header + 12);
    const auto value = page_wire::get32(header + 16);
    if (database >= page_batch::max_databases ||
        !valid_lengths(operation, key, value, protocol)) {
        return false;
    }
    try {
        page_frame frame;
        frame.protocol = protocol;
        frame.operation = operation;
        frame.database = database;
        if (!read_bytes(connection, key, frame.key)) {
            return false;
        }
        const bool page = operation == page_operation::page_write ||
            operation == page_operation::page_erase;
        if (page) {
            char number[8];
            if (connection->read(number, sizeof(number), true) !=
                sizeof(number)) {
                return false;
            }
            frame.id = page_wire::get64(number);
            if (frame.id == 0) {
                return false;
            }
        }
        if (!read_bytes(connection, value - (page ? 8 : 0), frame.value)) {
            return false;
        }
        out = std::move(frame);
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    }
}

} // namespace pkv
