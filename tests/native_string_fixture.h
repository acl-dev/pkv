#pragma once
#include "coder/string_page.h"
#include <stdexcept>

namespace pkv {

inline std::string native_string_fixture(const std::string& value) {
    std::string bytes;
    if (string_page_codec::encode_inline(-1, value, bytes, true) !=
        string_page_status::ok)
        throw std::runtime_error("invalid native String fixture");
    return bytes;
}

} // namespace pkv
