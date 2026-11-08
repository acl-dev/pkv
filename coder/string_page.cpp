#include "string_page.h"
#include <climits>
#include <cstring>
#include <new>
#include <utility>

namespace pkv {

const size_t string_page_codec::header_bytes;
const size_t string_page_codec::inline_bytes;
const size_t string_page_codec::page_bytes;
const uint64_t string_page_codec::max_bytes;
const uint64_t string_page_codec::write_bytes;

namespace {
constexpr size_t inline_header_bytes = 7;
constexpr size_t expiring_inline_header_bytes = inline_header_bytes + 8;

void number(char* bytes, uint64_t value, unsigned length) {
    for (unsigned i = length; i; --i)
        *bytes++ = static_cast<char>(value >> ((i - 1) * 8));
}
uint64_t number(const std::string& bytes, size_t offset, unsigned length) {
    uint64_t value = 0;
    for (unsigned i = 0; i < length; ++i)
        value = (value << 8) | static_cast<unsigned char>(bytes[offset + i]);
    return value;
}
void root_header(char* bytes, int64_t expires, uint64_t length,
    uint64_t pages) {
    std::memcpy(bytes, "PSR\1", 4);
    number(bytes + 4, pages ? 0 : 1, 4);
    number(bytes + 8, static_cast<uint64_t>(expires), 8);
    number(bytes + 16, length, 8); number(bytes + 24, pages, 8);
}
size_t inline_header(char* bytes, int64_t expires, size_t length) {
    std::memcpy(bytes, "PSR\2", 4);
    const bool expiring = expires >= 0;
    bytes[4] = expiring;
    number(bytes + 5, static_cast<uint64_t>(length), 2);
    if (!expiring) return inline_header_bytes;
    number(bytes + inline_header_bytes, static_cast<uint64_t>(expires), 8);
    return expiring_inline_header_bytes;
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

namespace {
string_page_status encode_root_bytes(int64_t expiry, uint64_t length,
    uint64_t pages, const std::string& value, std::string& out) {
    using codec = string_page_codec;
    try {
        char bytes[codec::header_bytes + codec::inline_bytes];
        size_t offset = codec::header_bytes;
        if (pages) root_header(bytes, expiry, length, pages);
        else offset = inline_header(bytes, expiry, value.size());
        // Capture the payload before assigning: out may alias value.
        if (!value.empty())
            std::memcpy(bytes + offset, value.data(), value.size());
        out.assign(bytes, offset + value.size());
        return string_page_status::ok;
    } catch (const std::bad_alloc&) {
        return string_page_status::out_of_memory;
    }
}
} // namespace

string_page_status string_page_codec::encode_root(const string_root& root,
    std::string& out) {
    if (!valid(root)) return string_page_status::invalid;
    return encode_root_bytes(root.expires_at_ms, root.length, root.pages,
        root.inline_value, out);
}

string_page_status string_page_codec::encode_inline(int64_t expires_at_ms,
    const std::string& value, std::string& out) {
    if (expires_at_ms < -1 || value.size() > inline_bytes)
        return string_page_status::invalid;
    return encode_root_bytes(expires_at_ms, value.size(), 0, value, out);
}

string_page_status string_page_codec::encode_integer(int64_t expires_at_ms,
    long long value, std::string& out) {
    if (expires_at_ms < -1) return string_page_status::invalid;
    static const char pairs[] =
        "00010203040506070809" "10111213141516171819"
        "20212223242526272829" "30313233343536373839"
        "40414243444546474849" "50515253545556575859"
        "60616263646566676869" "70717273747576777879"
        "80818283848586878889" "90919293949596979899";
    char bytes[expiring_inline_header_bytes + 20];
    char* end = bytes + sizeof(bytes);
    char* first = end;
    auto magnitude = static_cast<uint64_t>(value);
    if (value < 0) magnitude = uint64_t(0) - magnitude;
    while (magnitude >= 100) {
        const auto pair = static_cast<unsigned>(magnitude % 100);
        magnitude /= 100;
        *--first = pairs[pair * 2 + 1]; *--first = pairs[pair * 2];
    }
    if (magnitude < 10) *--first = static_cast<char>('0' + magnitude);
    else {
        *--first = pairs[magnitude * 2 + 1];
        *--first = pairs[magnitude * 2];
    }
    if (value < 0) *--first = '-';
    const auto size = static_cast<size_t>(end - first);
    const auto offset = inline_header(bytes, expires_at_ms, size);
    std::memmove(bytes + offset, first, size);
    try {
        out.assign(bytes, offset + size);
        return string_page_status::ok;
    } catch (const std::bad_alloc&) {
        return string_page_status::out_of_memory;
    }
}

string_page_status string_page_codec::decode_header(const std::string& bytes,
    string_root_header& out) {
    if (bytes.size() < 4 || std::memcmp(bytes.data(), "PSR", 3) != 0)
        return string_page_status::invalid;
    if (bytes[3] == 2) {
        if (bytes.size() < inline_header_bytes) return string_page_status::invalid;
        if (bytes[4] != 0 && bytes[4] != 1) return string_page_status::unsupported;
        const bool expiring = bytes[4] == 1;
        string_root_header root;
        root.payload_offset = expiring ? expiring_inline_header_bytes :
            inline_header_bytes;
        root.length = number(bytes, 5, 2);
        if (root.length > inline_bytes ||
            bytes.size() != root.payload_offset + root.length)
            return string_page_status::invalid;
        if (expiring) {
            const auto expiry = number(bytes, inline_header_bytes, 8);
            if (expiry > INT64_MAX) return string_page_status::invalid;
            root.expires_at_ms = static_cast<int64_t>(expiry);
        }
        out = root;
        return string_page_status::ok;
    }
    if (static_cast<unsigned char>(bytes[3]) != 1)
        return string_page_status::unsupported;
    if (bytes.size() < header_bytes) return string_page_status::invalid;
    const auto flags = number(bytes, 4, 4);
    if (flags > 1) return string_page_status::unsupported;
    const auto expiry = number(bytes, 8, 8);
    if (expiry != UINT64_MAX && expiry > INT64_MAX)
        return string_page_status::invalid;
    string_root_header root;
    root.payload_offset = header_bytes;
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
        root.inline_value.assign(bytes, header.payload_offset, std::string::npos);
        out = std::move(root);
        return string_page_status::ok;
    } catch (const std::bad_alloc&) {
        return string_page_status::out_of_memory;
    }
}

} // namespace pkv
