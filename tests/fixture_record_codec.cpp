#include "fixture_record_codec.h"
#include "rpc/page_wire.h"
#include <cstring>

namespace pkv {
namespace {
class fixture_view final : public record_view {
public:
    explicit fixture_view(value_record record)
    : record_view(record.header), record_(std::move(record)) {}

    bool native_importable() const override {
        return header().kind != value_kind::hash || !record_.hash_value.empty();
    }

    bool string_value(std::string& out) const override {
        return copy(value_kind::string, record_.string_value, out);
    }

    field_status hash_field(const std::string& key, std::string& out) const override {
        if (header().kind != value_kind::hash) return field_status::invalid;
        const auto it = record_.hash_value.find(key);
        if (it == record_.hash_value.end()) return field_status::missing;
        out = it->second;
        return field_status::found;
    }

    bool hash_values(std::map<std::string, std::string>& out) const override {
        return copy(value_kind::hash, record_.hash_value, out);
    }

    bool list_values(std::vector<std::string>& out) const override {
        return copy(value_kind::list, record_.list_value, out);
    }

    bool set_values(std::set<std::string>& out) const override {
        return copy(value_kind::set, record_.set_value, out);
    }

    bool zset_values(std::map<std::string, double>& out) const override {
        return copy(value_kind::zset, record_.zset_value, out);
    }

private:
    template<class T>
    bool copy(value_kind kind, const T& value, T& out) const {
        if (header().kind != kind) return false;
        out = value;
        return true;
    }

    value_record record_;
};

void append_number(std::string& out, uint64_t number) {
    char bytes[8];
    page_wire::put64(bytes, number);
    out.append(bytes, sizeof(bytes));
}

void append_string(std::string& out, const std::string& value) {
    append_number(out, value.size());
    out += value;
}

struct reader {
    const char* data;
    size_t left;

    bool number(uint64_t& out) {
        if (left < 8) return false;
        out = page_wire::get64(data);
        data += 8;
        left -= 8;
        return true;
    }

    bool string(std::string& out) {
        uint64_t size;
        if (!number(size) || size > left) return false;
        out.assign(data, static_cast<size_t>(size));
        data += size;
        left -= size;
        return true;
    }
};

bool decode_payload(reader& input, value_record& record) {
    if (record.header.kind == value_kind::string)
        return input.string(record.string_value);
    uint64_t count;
    if (!input.number(count) || count > input.left / 8) return false;
    for (uint64_t i = 0; i < count; ++i) {
        std::string key;
        if (!input.string(key)) return false;
        switch (record.header.kind) {
        case value_kind::hash: {
            std::string value;
            if (!input.string(value) || !record.hash_value.emplace(
                std::move(key), std::move(value)).second) return false;
            break;
        }
        case value_kind::list:
            record.list_value.emplace_back(std::move(key));
            break;
        case value_kind::set:
            if (!record.set_value.emplace(std::move(key)).second) return false;
            break;
        case value_kind::zset: {
            uint64_t bits;
            double score;
            if (!input.number(bits)) return false;
            std::memcpy(&score, &bits, sizeof(score));
            if (std::isnan(score) || !record.zset_value.emplace(
                std::move(key), score).second) return false;
            break;
        }
        default: return false;
        }
    }
    return true;
}
} // namespace

codec_status fixture_record_codec::decode(const std::string& bytes,
    std::unique_ptr<record_view>& out) const {
    out.reset();
    if (bytes.size() < 13 || bytes.compare(0, 4, "FIX1") != 0)
        return codec_status::invalid_record;
    try {
        value_record record;
        const auto kind = static_cast<unsigned char>(bytes[4]);
        if (kind >= static_cast<unsigned>(value_kind::stream))
            return codec_status::invalid_record;
        record.header.kind = static_cast<value_kind>(kind);
        reader input{bytes.data() + 5, bytes.size() - 5};
        uint64_t expiry;
        if (!input.number(expiry) ||
            (expiry != UINT64_MAX && expiry > LLONG_MAX))
            return codec_status::invalid_record;
        record.header.expires_at_ms = expiry == UINT64_MAX ? -1 :
            static_cast<long long>(expiry);
        if (!decode_payload(input, record) || input.left)
            return codec_status::invalid_record;
        out.reset(new fixture_view(std::move(record)));
        return codec_status::ok;
    } catch (const std::bad_alloc&) { return codec_status::out_of_memory; }
}

codec_status fixture_record_codec::encode(const value_record& record,
    std::string& out) const {
    try {
        std::string bytes("FIX1");
        bytes += static_cast<char>(record.header.kind);
        append_number(bytes, static_cast<uint64_t>(record.header.expires_at_ms));
        switch (record.header.kind) {
        case value_kind::string:
            append_string(bytes, record.string_value);
            break;
        case value_kind::hash:
            append_number(bytes, record.hash_value.size());
            for (const auto& item : record.hash_value) {
                append_string(bytes, item.first);
                append_string(bytes, item.second);
            }
            break;
        case value_kind::list:
            append_number(bytes, record.list_value.size());
            for (const auto& item : record.list_value) append_string(bytes, item);
            break;
        case value_kind::set:
            append_number(bytes, record.set_value.size());
            for (const auto& item : record.set_value) append_string(bytes, item);
            break;
        case value_kind::zset:
            append_number(bytes, record.zset_value.size());
            for (const auto& item : record.zset_value) {
                if (std::isnan(item.second)) return codec_status::invalid_record;
                append_string(bytes, item.first);
                uint64_t bits;
                std::memcpy(&bits, &item.second, sizeof(bits));
                append_number(bytes, bits);
            }
            break;
        case value_kind::stream: return codec_status::unsupported_version;
        }
        out.swap(bytes);
        return codec_status::ok;
    } catch (const std::bad_alloc&) { return codec_status::out_of_memory; }
}

// This object is linked only into fixtures that exercise injectable codecs.
// The server and the native codec tests use the production default factory.
std::shared_ptr<const record_codec> default_record_codec() {
    static const auto codec = std::make_shared<const fixture_record_codec>();
    return codec;
}

} // namespace pkv
