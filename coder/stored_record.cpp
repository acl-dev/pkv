#include "packed_hash.h"
#include "coder/compact_list_root.h"
#include "coder/compact_hash.h"
#include "compact_zset.h"
#include "compact_set.h"
#include "record_codec.h"
#include "list_page.h"
#include "map_page.h"
#include "zset_page.h"
#include "string_page.h"
#include "db/stream_store.h"
#include "common/execution_gate.h"
#include <new>

namespace pkv {

long long unix_time_ms() {
    return execution_time_ms();
}
namespace {

class paged_record_view : public record_view {
public:
    paged_record_view(const record_header& header, uint64_t elements)
    : record_view(header), elements_(elements) {}
    bool paged_payload() const override { return true; }
    bool valid_payload() const override { return false; }
    bool string_value(std::string&) const override { return false; }
    bool string_length(size_t& out) const override {
        return size(value_kind::string, out);
    }
    field_status hash_field(const std::string&, std::string&) const override {
        return field_status::invalid;
    }
    bool hash_values(std::map<std::string, std::string>&) const override {
        return false;
    }
    bool list_size(size_t& out) const override {
        return size(value_kind::list, out);
    }
    bool hash_size(size_t& out) const override {
        return size(value_kind::hash, out);
    }
    bool set_size(size_t& out) const override {
        return size(value_kind::set, out);
    }
    bool zset_size(size_t& out) const override {
        return size(value_kind::zset, out);
    }
private:
    const uint64_t elements_;
    bool size(value_kind kind, size_t& out) const {
        if (header().kind != kind ||
            elements_ > static_cast<uint64_t>(SIZE_MAX))
            return false;
        out = static_cast<size_t>(elements_);
        return true;
    }
};

} // namespace

namespace {
enum class native_kind { none, string, list, map, zset, stream };
native_kind native_root(const std::string& bytes) {
    if (bytes.size() < 3 || bytes[0] != 'P' || bytes[2] != 'R')
        return native_kind::none;
    switch (bytes[1]) {
    case 'S': return native_kind::string;
    case 'L': return native_kind::list;
    case 'M': return native_kind::map;
    case 'Z': return native_kind::zset;
    case 'X': return native_kind::stream;
    default: return native_kind::none;
    }
}
codec_status native_header(const std::string& bytes, native_kind kind,
    record_header& header, uint64_t& elements) {
    if (kind == native_kind::string) {
        string_root_header root;
        const auto result = string_page_codec::decode_header(bytes, root);
        if (result == string_page_status::unsupported)
            return codec_status::unsupported_version;
        if (result != string_page_status::ok)
            return codec_status::invalid_record;
        header.kind = value_kind::string;
        header.expires_at_ms = root.expires_at_ms; elements = root.length;
    } else if (kind == native_kind::list) {
        if (compact_list_root::matches(bytes)) {
            try {
                header.kind = value_kind::list;
                const compact_list_root::view packed(bytes);
                elements = packed.count();
                header.expires_at_ms = packed.deadline();
                return codec_status::ok;
            } catch (const std::runtime_error&) { return codec_status::invalid_record; }
        }
        list_root root;
        const auto result = list_page_codec::decode_root(bytes, root);
        if (result == list_page_status::unsupported_version)
            return codec_status::unsupported_version;
        if (result != list_page_status::ok || !root.elements)
            return codec_status::invalid_record;
        header.kind = value_kind::list;
        header.expires_at_ms = root.expires_at_ms; elements = root.elements;
    } else if (kind == native_kind::map) {
        if (compact_set::matches(bytes)) {
            try {
                const compact_set::view packed(bytes);
                header.kind = value_kind::set;
                header.expires_at_ms = packed.deadline();
                elements = packed.count();
                return codec_status::ok;
            } catch (const std::runtime_error&) {
                return codec_status::invalid_record;
            }
        }
        if (compact_hash::matches(bytes)) {
            try {
                header.kind = value_kind::hash;
                const compact_hash::view packed(bytes);
                elements = packed.count();
                header.expires_at_ms = packed.deadline();
                return codec_status::ok;
            } catch (const std::runtime_error&) {
                return codec_status::invalid_record;
            }
        }
        if (packed_hash::matches(bytes)) {
            packed_hash::root root;
            if (packed_hash::decode_root(bytes, root) != map_page_status::ok)
                return codec_status::invalid_record;
            header.kind = value_kind::hash;
            header.expires_at_ms = root.expires_at_ms; elements = root.elements;
            return codec_status::ok;
        }
        map_root root;
        const auto result = map_page_codec::decode_root(bytes, root);
        if (result == map_page_status::unsupported)
            return codec_status::unsupported_version;
        if (result != map_page_status::ok || !root.elements)
            return codec_status::invalid_record;
        header.kind = root.kind == map_kind::hash ? value_kind::hash :
            value_kind::set;
        header.expires_at_ms = root.expires_at_ms; elements = root.elements;
    } else if (kind == native_kind::stream) {
        if (bytes.size() >= 4 && bytes[3] != 1 && bytes[3] != 2 && bytes[3] != 3)
            return codec_status::unsupported_version;
        stream_root root;
        if (!decode_stream_root(bytes, root)) return codec_status::invalid_record;
        header.kind = value_kind::stream;
        header.expires_at_ms = root.expires_at_ms; elements = root.size;
    } else {
        if (compact_zset::matches(bytes)) {
            try {
                const compact_zset::view packed(bytes);
                packed.validate_fast();
                header.kind = value_kind::zset;
                header.expires_at_ms = packed.deadline();
                elements = packed.count();
                return codec_status::ok;
            } catch (const std::runtime_error&) { return codec_status::invalid_record; }
        }
        zset_root root;
        const auto result = zset_page_codec::decode_root(bytes, root);
        if (result == map_page_status::unsupported)
            return codec_status::unsupported_version;
        if (result != map_page_status::ok || !root.elements)
            return codec_status::invalid_record;
        header.kind = value_kind::zset;
        header.expires_at_ms = root.expires_at_ms; elements = root.elements;
    }
    return codec_status::ok;
}
} // namespace

codec_status record_codec::open_stored(const std::string& bytes,
    record_header& header, std::unique_ptr<record_view>& legacy,
    bool& native) const {
    header = record_header();
    const auto kind = native_root(bytes);
    native = kind != native_kind::none;
    if (native) {
        legacy.reset();
        uint64_t elements;
        return native_header(bytes, kind, header, elements);
    }
    const auto status = decode(bytes, legacy);
    if (status == codec_status::ok && legacy) header = legacy->header();
    return status;
}

codec_status record_codec::stored_header(const std::string& bytes,
    record_header& out) const {
    out = record_header();
    const auto kind = native_root(bytes);
    if (kind != native_kind::none) {
        uint64_t elements;
        return native_header(bytes, kind, out, elements);
    }
    std::unique_ptr<record_view> view;
    const auto status = decode(bytes, view);
    if (status != codec_status::ok) return status;
    if (!view) return codec_status::invalid_record;
    out = view->header(); return codec_status::ok;
}

codec_status record_codec::decode_stored(const std::string& bytes,
    std::unique_ptr<record_view>& out) const {
    const auto kind = native_root(bytes);
    if (kind == native_kind::none) return decode(bytes, out);
    out.reset();
    record_header header;
    uint64_t elements;
    const auto status = native_header(bytes, kind, header, elements);
    if (status != codec_status::ok) return status;
    try {
        out.reset(new paged_record_view(header, elements));
        return codec_status::ok;
    } catch (const std::bad_alloc&) {
        return codec_status::out_of_memory;
    }
}

} // namespace pkv
