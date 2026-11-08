#include "stdafx.h"
#include "page_stream.h"
#include "page_wire.h"
#include <new>

namespace pkv {
namespace {

// Borrow payloads from the batch and buffer only frame headers and vectors.
class frame_writer {
public:
    explicit frame_writer(acl::shared_stream& connection)
        : connection_(connection) {}

    bool append(page_operation operation, unsigned protocol, unsigned database,
        const std::string& key, const char* value, size_t length, uint64_t id = 0,
        bool copy_value = false) {
        if (frames_ == capacity && !flush()) return false;
        const bool page = operation == page_operation::page_write ||
            operation == page_operation::page_erase;
        auto& header = headers_[frames_];
        auto& number = numbers_[frames_++];
        page_wire::put32(header, page_wire::magic_for(protocol));
        page_wire::put32(header + 4, static_cast<uint32_t>(operation));
        page_wire::put32(header + 8, database);
        page_wire::put32(header + 12, static_cast<uint32_t>(key.size()));
        page_wire::put32(header + 16,
            static_cast<uint32_t>(length + (page ? sizeof(number) : 0)));
        add(header, sizeof(header));
        if (!key.empty()) add(key.data(), key.size());
        if (page) {
            page_wire::put64(number, id);
            add(number, sizeof(number));
        }
        if (copy_value) {
            auto& payload = payloads_[frames_ - 1];
            assert(length <= sizeof(payload));
            std::memcpy(payload, value, length);
            value = payload;
        }
        if (length) add(value, length);
        return true;
    }

    bool flush() {
        if (!vectors_used_) return true;
        if (connection_->writev(vectors_, vectors_used_) != static_cast<int>(bytes_))
            return false;
        vectors_used_ = frames_ = 0;
        bytes_ = 0;
        return true;
    }

private:
    void add(const char* data, size_t size) {
        vectors_[vectors_used_++] = {const_cast<char*>(data), size};
        bytes_ += size;
    }

    static constexpr int capacity = 16;
    acl::shared_stream& connection_;
    char headers_[capacity][20], numbers_[capacity][8], payloads_[capacity][16];
    iovec vectors_[capacity * 4];
    int frames_ = 0, vectors_used_ = 0;
    size_t bytes_ = 0;
};

bool valid_lengths(page_operation operation, size_t key, size_t value,
    unsigned protocol) {
    if (key > page_batch::max_key_bytes) {
        return false;
    }
    switch (operation) {
    case page_operation::exec_begin:
        return (protocol >= 3 && protocol <= 5) && key == 0 && value == 16;
    case page_operation::exec_commit:
        return (protocol >= 3 && protocol <= 5) && key == 0 && value == 0;
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
    case page_operation::page_erase_group:
        return protocol == 5 && value >= 16 && value % 8 == 0 &&
            value / 8 <= page_batch::max_erase_ids;
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

bool append_batch(frame_writer& writer, const page_batch& batch,
    unsigned protocol) {
    if (protocol < batch.required_protocol() || protocol > 5) return false;
    if (protocol == 5 && !batch.size().compact) protocol = 4;
    char begin[16];
    page_wire::put32(begin, static_cast<uint32_t>(batch.size().roots));
    page_wire::put32(begin + 4, static_cast<uint32_t>(batch.size().pages));
    page_wire::put64(begin + 8, batch.size().bytes);
    const std::string empty;
    const auto database = batch.database();
    if (!writer.append(page_operation::begin, protocol, database,
        empty, begin, sizeof(begin), 0, true)) {
        return false;
    }
    for (const auto& root : batch.roots()) {
        const auto operation = root.action == db::update_action::write ?
            (root.maintenance_only ? page_operation::root_maintenance :
                root.clear_pages ? page_operation::root_replace :
                page_operation::root_write) :
                root.action == db::update_action::erase ?
                    page_operation::root_erase : page_operation::root_keep;
        if (!writer.append(operation, protocol, database, root.key,
            root.value.data(), root.action == db::update_action::write ?
                root.value.size() : 0)) {
            return false;
        }
    }
    const auto& pages = batch.pages();
    for (size_t at = 0; at < pages.size();) {
        const auto& page = pages[at];
        const auto count = batch.size().compact ?
            page_batch::erase_run(pages, at) : 1;
        if (count > 1) {
            std::string ids(count * 8, '\0');
            for (size_t i = 0; i < count; ++i)
                page_wire::put64(&ids[i * 8], pages[at + i].id);
            // The temporary ID buffer must stay alive through the write.
            if (!writer.append(page_operation::page_erase_group,
                protocol, database, page.owner, ids.data(), ids.size()) ||
                !writer.flush())
                return false;
        } else if (!writer.append(
            page.action == db::update_action::write ?
                page_operation::page_write : page_operation::page_erase,
            protocol, database, page.owner, page.value.data(),
            page.value.size(), page.id)) return false;
        at += count;
    }
    return writer.append(page_operation::commit, protocol,
        database, empty, nullptr, 0);
}

} // namespace

bool page_stream::send(acl::shared_stream& connection,
    const page_batch& batch, unsigned protocol) {
    frame_writer writer(connection);
    return append_batch(writer, batch, protocol) && writer.flush();
}

bool page_stream::send_run(acl::shared_stream& connection,
    const page_batch* const* batches, size_t count, unsigned protocol) {
    frame_writer writer(connection);
    for (size_t i = 0; i < count; ++i)
        if (!append_batch(writer, *batches[i], protocol)) return false;
    return writer.flush();
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
        const bool page = operation == page_operation::page_write ||
            operation == page_operation::page_erase;
        char body[512];
        const auto length = key + value;
        if (length <= sizeof(body)) {
            if (length && connection->read(body, length, true) !=
                static_cast<int>(length)) return false;
            const auto id_bytes = page ? 8 : 0;
            if (page) {
                frame.id = page_wire::get64(body + key);
                if (!frame.id) return false;
            }
            frame.key.assign(body, key);
            frame.value.assign(body + key + id_bytes, value - id_bytes);
            out = std::move(frame);
            return true;
        }
        if (!read_bytes(connection, key, frame.key)) {
            return false;
        }
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
