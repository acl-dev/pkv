#include "stream_store.h"
#include "rpc/page_wire.h"
#include "coder/record_codec.h"
#include <algorithm>
#include <cstring>

namespace pkv {
namespace {
using status = stream_store::status;
constexpr size_t root_bytes = 36;
constexpr size_t compact_root_bytes = 16;
constexpr size_t compact_expiring_root_bytes = compact_root_bytes + 6;
constexpr uint32_t root_expiry_flag = uint32_t(1) << 31;
constexpr uint64_t short_timestamp_mask = (uint64_t(1) << 48) - 1;
constexpr size_t entry_header_bytes = 20;
constexpr size_t fragment_header_bytes = entry_header_bytes + 4;
constexpr size_t compact_header_bytes = 10;
constexpr size_t compact_payload_bytes = 12;
constexpr size_t compact_entry_bytes = compact_header_bytes + compact_payload_bytes;
constexpr uint64_t fragment_base = uint64_t(1) << 63;
constexpr uint64_t fragment_position_limit = (fragment_base - 1) >> 16;
static_assert(stream_store::max_entry_bytes <=
    size_t(65536) * page_transaction::max_value_bytes, "fragment index overflow");

// Entry headers retain their dense low-half positions. The high half holds
// fragments, keyed by entry position and zero-based fragment number.
uint64_t fragment_id(uint64_t position, size_t index) {
    return fragment_base | (position << 16) | index;
}

bool later(stream_id a, stream_id b) {
    return a.milliseconds > b.milliseconds ||
        (a.milliseconds == b.milliseconds && a.sequence > b.sequence);
}

size_t encoded_root_size(const stream_root& root) {
    if (!root.compact_entries || root.size >= root_expiry_flag ||
        root.last.milliseconds > short_timestamp_mask ||
        root.last.sequence > UINT16_MAX || root.expires_at_ms < -1 ||
        (root.expires_at_ms >= 0 &&
            static_cast<uint64_t>(root.expires_at_ms) > short_timestamp_mask))
        return root_bytes;
    return root.expires_at_ms >= 0 ? compact_expiring_root_bytes : compact_root_bytes;
}

status encode_entry(stream_id id, const std::string* const* fields,
    size_t count, std::string& out) {
    const auto limit = stream_store::max_entry_bytes;
    size_t size = entry_header_bytes;
    if (!fields || !count || count % 2) return status::invalid;
    if (count == 2 && fields[0] && fields[1] &&
        fields[0]->size() <= compact_payload_bytes &&
        fields[1]->size() <= compact_payload_bytes - fields[0]->size() &&
        id.milliseconds < (uint64_t(1) << 48) && id.sequence <= UINT16_MAX) {
        char data[compact_entry_bytes];
        // The 64-bit store places the six timestamp bytes at offsets 1–6.
        page_wire::put64(data, id.milliseconds << 8);
        data[0] = 1;
        data[7] = static_cast<char>(id.sequence >> 8);
        data[8] = static_cast<char>(id.sequence);
        const auto key_size = fields[0]->size();
        const auto value_size = fields[1]->size();
        data[9] = static_cast<char>((key_size << 4) | value_size);
        std::memcpy(data + compact_header_bytes, fields[0]->data(), key_size);
        std::memcpy(data + compact_header_bytes + key_size, fields[1]->data(), value_size);
        out.assign(data, compact_header_bytes + key_size + value_size);
        return status::ok;
    }
    for (size_t i = 0; i < count; ++i) {
        const auto* value = fields[i];
        if (!value) return status::invalid;
        if (limit - size < 4 || value->size() > limit - size - 4)
            return status::too_large;
        size += 4 + value->size();
    }
    out.reserve(size);
    char header[entry_header_bytes];
    page_wire::put64(header, id.milliseconds);
    page_wire::put64(header + 8, id.sequence);
    page_wire::put32(header + 16, static_cast<uint32_t>(count / 2));
    out.append(header, sizeof(header));
    for (size_t i = 0; i < count; ++i) {
        char length[4];
        page_wire::put32(length, static_cast<uint32_t>(fields[i]->size()));
        out.append(length, sizeof(length));
        out.append(*fields[i]);
    }
    return status::ok;
}

bool read_field(const std::string& bytes, size_t& offset, std::string* out) {
    if (bytes.size() - offset < 4) return false;
    const auto length = page_wire::get32(bytes.data() + offset);
    offset += 4;
    if (length > bytes.size() - offset) return false;
    if (out) out->assign(bytes, offset, length);
    offset += length;
    return true;
}
} // namespace

bool decode_stream_root(const std::string& bytes, stream_root& out) {
    if (bytes.size() < 4 || bytes.compare(0, 3, "PXR") != 0)
        return false;
    stream_root root;
    uint64_t expiry = UINT64_MAX;
    if (bytes[3] == 3) {
        if (bytes.size() < compact_root_bytes) return false;
        const auto count = page_wire::get32(bytes.data() + 4);
        const bool expiring = (count & root_expiry_flag) != 0;
        if (bytes.size() != (expiring ? compact_expiring_root_bytes : compact_root_bytes))
            return false;
        const auto id = page_wire::get64(bytes.data() + 8);
        root.size = count & ~root_expiry_flag;
        root.last.milliseconds = id >> 16;
        root.last.sequence = id & UINT16_MAX;
        root.compact_entries = true;
        if (expiring)
            expiry = page_wire::get64(bytes.data() + 14) & short_timestamp_mask;
    } else {
        if (bytes.size() != root_bytes || (bytes[3] != 1 && bytes[3] != 2))
            return false;
        root.compact_entries = bytes[3] == 2;
        root.size = page_wire::get64(bytes.data() + 4);
        root.last.milliseconds = page_wire::get64(bytes.data() + 12);
        root.last.sequence = page_wire::get64(bytes.data() + 20);
        expiry = page_wire::get64(bytes.data() + 28);
    }
    if (!root.size || root.size > INT64_MAX || !later(root.last, stream_id()) ||
        (expiry != UINT64_MAX && expiry > INT64_MAX)) return false;
    root.expires_at_ms = expiry == UINT64_MAX ? -1 : static_cast<int64_t>(expiry);
    out = root;
    return true;
}

void encode_stream_root(const stream_root& root, std::string& out) {
    char bytes[root_bytes];
    const auto size = encoded_root_size(root);
    if (size != root_bytes) {
        std::memcpy(bytes, "PXR\3", 4);
        const bool expiring = root.expires_at_ms >= 0;
        page_wire::put32(bytes + 4, static_cast<uint32_t>(root.size) |
            (expiring ? root_expiry_flag : 0));
        page_wire::put64(bytes + 8, (root.last.milliseconds << 16) | root.last.sequence);
        if (expiring) {
            char expiry[8];
            page_wire::put64(expiry, static_cast<uint64_t>(root.expires_at_ms));
            std::memcpy(bytes + compact_root_bytes, expiry + 2, 6);
        }
        out.assign(bytes, size);
        return;
    }
    std::memcpy(bytes, "PXR\1", 4);
    if (root.compact_entries) bytes[3] = 2;
    page_wire::put64(bytes + 4, root.size);
    page_wire::put64(bytes + 12, root.last.milliseconds);
    page_wire::put64(bytes + 20, root.last.sequence);
    page_wire::put64(bytes + 28, static_cast<uint64_t>(root.expires_at_ms));
    out.assign(bytes, sizeof(bytes));
}

stream_store::stream_store(page_transaction& transaction, const std::string& key)
    : transaction_(transaction), root_(transaction.roots().find(key)) {}

stream_store::status stream_store::open(bool expire) {
    if (!root_ || opened_) return status::invalid;
    if ((root_->found || root_->action == db::update_action::write) &&
        root_->action != db::update_action::erase) {
        if (!decode_stream_root(root_->value, state_)) return status::invalid;
        if (expire && state_.expires_at_ms >= 0 &&
            state_.expires_at_ms <= unix_time_ms()) {
            root_->action = db::update_action::erase;
            state_ = stream_root();
        }
    }
    opened_ = true;
    return status::ok;
}

stream_store::status stream_store::append(stream_id id,
    const std::string* const* fields, size_t count) {
    if (!opened_) return status::invalid;
    if (!later(id, state_.last)) return status::invalid_id;
    if (state_.size == INT64_MAX) return status::too_large;
    std::string bytes;
    const auto encoded = encode_entry(id, fields, count, bytes);
    if (encoded != status::ok) return encoded;
    auto next = state_;
    ++next.size;
    next.last = id;
    next.compact_entries = next.compact_entries || bytes.size() <= compact_entry_bytes;
    if (bytes.size() > page_transaction::max_value_bytes &&
        next.size > fragment_position_limit) return status::too_large;
    // Reserve before changing the transaction. Established streams already
    // have room for the fixed-size header in the root workspace.
    root_->value.reserve(encoded_root_size(next));
    if (!state_.size && !transaction_.reset_pages(root_->key)) return status::storage;
    // Set write before staging a page when replacing an erased root.
    root_->action = db::update_action::write;
    const auto staged = stage_entry(next.size, std::move(bytes));
    if (staged != status::ok) return staged;
    encode_stream_root(next, root_->value);
    state_ = next;
    return status::ok;
}

stream_store::status stream_store::stage_entry(uint64_t position,
    std::string&& bytes) {
    if (bytes.size() <= page_transaction::max_value_bytes) {
        if (!transaction_.put(root_->key, position, std::move(bytes)))
            return status::storage;
    } else {
        // A zero field count distinguishes a fragment descriptor from every
        // inline record. Its ID remains available to range binary searches.
        std::string descriptor(bytes, 0, entry_header_bytes);
        page_wire::put32(&descriptor[16], 0);
        char length[4];
        page_wire::put32(length, static_cast<uint32_t>(bytes.size()));
        descriptor.append(length, sizeof(length));
        if (!transaction_.put(root_->key, position, std::move(descriptor)))
            return status::storage;
        const auto chunk = page_transaction::max_value_bytes;
        for (size_t offset = 0; offset < bytes.size(); offset += chunk) {
            if (!transaction_.put(root_->key, fragment_id(position, offset / chunk),
                bytes.substr(offset, chunk))) return status::storage;
        }
    }
    return status::ok;
}

stream_store::status stream_store::read_page(uint64_t page, std::string& bytes) {
    auto copy = [](void* context, const std::string& input) {
        auto& output = *static_cast<std::string*>(context);
        // Backends without a borrowed reader may already use this buffer.
        if (&output != &input) output = input;
    };
    const auto found = transaction_.inspect_bound(root_, page, bytes, copy,
        &bytes);
    if (found == db::lookup_status::error) return status::storage;
    return found == db::lookup_status::found ? status::ok : status::invalid;
}

stream_store::status stream_store::read_header(uint64_t position,
    std::string& bytes, stream_id& id, uint32_t& fields) {
    if (!opened_ || !position || position > state_.size) return status::invalid;
    const auto read = read_page(position, bytes);
    if (read != status::ok) return read;
    return decode_header(position, bytes, id, fields);
}

stream_store::status stream_store::decode_header(uint64_t position,
    const std::string& bytes, stream_id& id, uint32_t& fields) const {
    if (bytes.size() <= compact_entry_bytes) {
        if (!state_.compact_entries || bytes.size() < compact_header_bytes ||
            bytes[0] != 1) return status::invalid;
        const auto lengths = static_cast<uint8_t>(bytes[9]);
        if (bytes.size() != compact_header_bytes + (lengths >> 4) + (lengths & 15))
            return status::invalid;
        id.milliseconds = (page_wire::get64(bytes.data()) >> 8) &
            ((uint64_t(1) << 48) - 1);
        id.sequence = (static_cast<uint8_t>(bytes[7]) << 8) |
            static_cast<uint8_t>(bytes[8]);
        fields = 1;
        return later(id, stream_id()) && !later(id, state_.last) &&
            (position != state_.size || !later(state_.last, id)) ?
            status::ok : status::invalid;
    }
    if (bytes.size() < entry_header_bytes) return status::invalid;
    id.milliseconds = page_wire::get64(bytes.data());
    id.sequence = page_wire::get64(bytes.data() + 8);
    fields = page_wire::get32(bytes.data() + 16);
    if (!later(id, stream_id()) || later(id, state_.last) ||
        (position == state_.size && later(state_.last, id))) return status::invalid;
    if (fields) {
        if (fields > (bytes.size() - entry_header_bytes) / 8) return status::invalid;
    } else {
        if (bytes.size() != fragment_header_bytes || position > fragment_position_limit)
            return status::invalid;
        const auto length = page_wire::get32(bytes.data() + entry_header_bytes);
        if (length <= page_transaction::max_value_bytes || length > max_entry_bytes)
            return status::invalid;
    }
    return status::ok;
}

stream_store::status stream_store::read_encoded(uint64_t position,
    std::string& bytes, stream_id& id, uint32_t& count) {
    const auto result = read_header(position, bytes, id, count);
    if (result != status::ok) return result;
    if (!count) {
        const auto length = page_wire::get32(bytes.data() + entry_header_bytes);
        bytes.clear();
        bytes.reserve(length);
        std::string fragment;
        const auto chunk = page_transaction::max_value_bytes;
        for (size_t offset = 0; offset < length; offset += chunk) {
            const auto read = read_page(fragment_id(position, offset / chunk),
                fragment);
            if (read != status::ok) return read;
            if (fragment.size() != std::min(chunk, size_t(length) - offset))
                return status::invalid;
            bytes.append(fragment);
        }
        stream_id decoded_id;
        if (decode_header(position, bytes, decoded_id, count) != status::ok ||
            !count || later(id, decoded_id) || later(decoded_id, id))
            return status::invalid;
    }
    return status::ok;
}

stream_store::status stream_store::read(uint64_t position, stream_entry& entry) {
    std::string bytes;
    stream_entry value;
    uint32_t count;
    const auto result = read_encoded(position, bytes, value.id, count);
    if (result != status::ok) return result;
    if (bytes.size() <= compact_entry_bytes) {
        const auto lengths = static_cast<uint8_t>(bytes[9]);
        const auto key_size = lengths >> 4;
        value.fields.emplace_back(bytes.substr(compact_header_bytes, key_size),
            bytes.substr(compact_header_bytes + key_size));
        entry = std::move(value);
        return status::ok;
    }
    size_t offset = entry_header_bytes;
    value.fields.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        std::pair<std::string, std::string> field;
        if (!read_field(bytes, offset, &field.first) ||
            !read_field(bytes, offset, &field.second)) return status::invalid;
        value.fields.push_back(std::move(field));
    }
    if (offset != bytes.size()) return status::invalid;
    entry = std::move(value);
    return status::ok;
}

stream_store::status stream_store::range(stream_id minimum, stream_id maximum,
    bool reverse, uint64_t limit, std::vector<stream_entry>& out) {
    if (!opened_) return status::invalid;
    if (!limit || !state_.size || later(minimum, maximum)) {
        out.clear();
        return status::ok;
    }
    // Find the first candidate by position, without decoding field payloads.
    uint64_t low = 1, high = state_.size + 1;
    if (reverse && !later(state_.last, maximum)) low = high;
    else if (!reverse && !later(minimum, stream_id())) high = low;
    std::string scratch;
    while (low < high) {
        const auto mid = low + (high - low) / 2;
        struct probe {
            const stream_store& store;
            uint64_t position;
            stream_id id;
            status result;
        } state{*this, mid, stream_id(), status::invalid};
        auto decode = [](void* raw, const std::string& bytes) {
            auto& work = *static_cast<probe*>(raw);
            uint32_t fields;
            work.result = work.store.decode_header(work.position, bytes,
                work.id, fields);
        };
        const auto found = transaction_.inspect_bound(root_, mid, scratch,
            decode, &state);
        if (found == db::lookup_status::error) return status::storage;
        if (state.result != status::ok) return state.result;
        const bool before = reverse ? !later(state.id, maximum) :
            later(minimum, state.id);
        if (before) low = mid + 1;
        else high = mid;
    }
    std::vector<stream_entry> values;
    for (auto position = reverse ? low - 1 : low;
         position && position <= state_.size && values.size() < limit;
         position = reverse ? position - 1 : position + 1) {
        stream_entry entry;
        const auto result = read(position, entry);
        if (result != status::ok) return result;
        if (later(minimum, entry.id) || later(entry.id, maximum)) break;
        values.push_back(std::move(entry));
    }
    out.swap(values);
    return status::ok;
}

stream_store::status stream_store::clone_to(stream_store& destination) {
    if (!opened_ || !state_.size || !destination.root_ ||
        root_ == destination.root_) return status::invalid;
    const auto& key = destination.root_->key;
    auto& tx = destination.transaction_;
    if (!tx.reset_pages(key)) return status::storage;
    destination.root_->action = db::update_action::write;
    const auto reject = [&](status error) { tx.abort(); return error; };
    stream_id previous;
    std::string bytes;
    for (uint64_t position = 1; position <= state_.size; ++position) {
        stream_id id;
        uint32_t count;
        const auto read = read_encoded(position, bytes, id, count);
        if (read != status::ok) return reject(read);
        if (!later(id, previous)) return reject(status::invalid);
        previous = id;
        if (bytes.size() > compact_entry_bytes) {
            size_t offset = entry_header_bytes;
            for (uint32_t i = 0; i < count * 2; ++i)
                if (!read_field(bytes, offset, nullptr)) return reject(status::invalid);
            if (offset != bytes.size()) return reject(status::invalid);
        }
        if (bytes.size() <= page_transaction::max_value_bytes) {
            if (!tx.put_tail_bound(destination.root_, position, std::move(bytes)))
                return reject(status::storage);
        } else {
            const auto staged = destination.stage_entry(position, std::move(bytes));
            if (staged != status::ok) return reject(staged);
        }
    }
    encode_stream_root(state_, destination.root_->value);
    destination.state_ = state_;
    destination.opened_ = true;
    return status::ok;
}

} // namespace pkv
