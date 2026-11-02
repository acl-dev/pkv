#include "string_page.h"
#include <climits>
#include <new>
#include <utility>

namespace pkv {

const size_t string_page_codec::header_bytes;
const size_t string_page_codec::inline_bytes;
const size_t string_page_codec::page_bytes;
const uint64_t string_page_codec::max_bytes;
const uint64_t string_page_codec::write_bytes;

namespace {
void number(std::string& bytes, uint64_t value, unsigned length) {
    for (unsigned i = length; i; --i)
        bytes.push_back(static_cast<char>(value >> ((i - 1) * 8)));
}
uint64_t number(const std::string& bytes, size_t offset, unsigned length) {
    uint64_t value = 0;
    for (unsigned i = 0; i < length; ++i)
        value = (value << 8) | static_cast<unsigned char>(bytes[offset + i]);
    return value;
}
bool valid(const string_root& root) {
    if (root.expires_at_ms < -1 ||
        root.length > string_page_codec::max_bytes) return false;
    return root.length <= string_page_codec::inline_bytes ?
        root.pages == 0 && root.inline_value.size() == root.length :
        root.pages == string_page_codec::pages_for(root.length) &&
            root.inline_value.empty();
}
}

uint64_t string_page_codec::pages_for(uint64_t length) {
    return length <= inline_bytes ? 0 : (length - 1) / page_bytes + 1;
}
size_t string_page_codec::page_size(const string_root& root, uint64_t index) {
    if (index >= root.pages) return 0;
    return index + 1 == root.pages ?
        static_cast<size_t>(root.length - index * page_bytes) : page_bytes;
}

string_page_status string_page_codec::encode_root(const string_root& root,
    std::string& out) {
    if (!valid(root)) return string_page_status::invalid;
    try {
        std::string bytes("PSR\1", 4);
        number(bytes, root.pages ? 0 : 1, 4);
        number(bytes, static_cast<uint64_t>(root.expires_at_ms), 8);
        number(bytes, root.length, 8); number(bytes, root.pages, 8);
        bytes += root.inline_value;
        out.swap(bytes);
        return string_page_status::ok;
    } catch (const std::bad_alloc&) {
        return string_page_status::out_of_memory;
    }
}

string_page_status string_page_codec::decode_header(const std::string& bytes,
    string_root_header& out) {
    if (bytes.size() < 4 || bytes.compare(0, 3, "PSR") != 0)
        return string_page_status::invalid;
    if (static_cast<unsigned char>(bytes[3]) != 1)
        return string_page_status::unsupported;
    if (bytes.size() < header_bytes) return string_page_status::invalid;
    const auto flags = number(bytes, 4, 4);
    if (flags > 1) return string_page_status::unsupported;
    const auto expiry = number(bytes, 8, 8);
    if (expiry != UINT64_MAX && expiry > INT64_MAX)
        return string_page_status::invalid;
    string_root_header root;
    root.expires_at_ms = expiry == UINT64_MAX ? -1 :
        static_cast<int64_t>(expiry);
    root.length = number(bytes, 16, 8);
    root.pages = number(bytes, 24, 8);
    if ((flags == 1) != (root.pages == 0) || root.length > max_bytes ||
        root.pages != pages_for(root.length))
        return string_page_status::invalid;
    const auto inline_size = root.pages ? 0 : root.length;
    if (bytes.size() - header_bytes != inline_size)
        return string_page_status::invalid;
    out = root; return string_page_status::ok;
}
string_page_status string_page_codec::decode_root(const std::string& bytes,
    string_root& out) {
    string_root_header header;
    const auto status = decode_header(bytes, header);
    if (status != string_page_status::ok) return status;
    try {
        string_root root;
        root.expires_at_ms = header.expires_at_ms;
        root.length = header.length; root.pages = header.pages;
        root.inline_value.assign(bytes, header_bytes, std::string::npos);
        out = std::move(root);
        return string_page_status::ok;
    } catch (const std::bad_alloc&) {
        return string_page_status::out_of_memory;
    }
}

} // namespace pkv
