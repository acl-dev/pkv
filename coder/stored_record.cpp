#include "record_codec.h"
#include "list_page.h"
#include "map_page.h"
#include "zset_page.h"
#include "string_page.h"
#include <new>

namespace pkv {
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
bool native_root(const std::string& bytes) {
    return bytes.compare(0, 3, "PLR") == 0 ||
        bytes.compare(0, 3, "PMR") == 0 ||
        bytes.compare(0, 3, "PZR") == 0 ||
        bytes.compare(0, 3, "PSR") == 0;
}
codec_status native_header(const std::string& bytes, record_header& header,
    uint64_t& elements) {
    if (bytes.compare(0, 3, "PSR") == 0) {
        string_root_header root;
        const auto result = string_page_codec::decode_header(bytes, root);
        if (result == string_page_status::unsupported)
            return codec_status::unsupported_version;
        if (result != string_page_status::ok)
            return codec_status::invalid_record;
        header.kind = value_kind::string;
        header.expires_at_ms = root.expires_at_ms; elements = root.length;
    } else if (bytes.compare(0, 3, "PLR") == 0) {
        list_root root;
        const auto result = list_page_codec::decode_root(bytes, root);
        if (result == list_page_status::unsupported_version)
            return codec_status::unsupported_version;
        if (result != list_page_status::ok || !root.elements)
            return codec_status::invalid_record;
        header.kind = value_kind::list;
        header.expires_at_ms = root.expires_at_ms; elements = root.elements;
    } else if (bytes.compare(0, 3, "PMR") == 0) {
        map_root root;
        const auto result = map_page_codec::decode_root(bytes, root);
        if (result == map_page_status::unsupported)
            return codec_status::unsupported_version;
        if (result != map_page_status::ok || !root.elements)
            return codec_status::invalid_record;
        header.kind = root.kind == map_kind::hash ? value_kind::hash :
            value_kind::set;
        header.expires_at_ms = root.expires_at_ms; elements = root.elements;
    } else {
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

codec_status record_codec::stored_header(const std::string& bytes,
    record_header& out) const {
    out = record_header();
    if (native_root(bytes)) {
        uint64_t elements;
        return native_header(bytes, out, elements);
    }
    std::unique_ptr<record_view> view;
    const auto status = decode(bytes, view);
    if (status != codec_status::ok) return status;
    if (!view) return codec_status::invalid_record;
    out = view->header(); return codec_status::ok;
}

codec_status record_codec::decode_stored(const std::string& bytes,
    std::unique_ptr<record_view>& out) const {
    if (!native_root(bytes)) return decode(bytes, out);
    out.reset();
    record_header header;
    uint64_t elements;
    const auto status = native_header(bytes, header, elements);
    if (status != codec_status::ok) return status;
    try {
        out.reset(new paged_record_view(header, elements));
        return codec_status::ok;
    } catch (const std::bad_alloc&) {
        return codec_status::out_of_memory;
    }
}

} // namespace pkv
