#include "coder/record_codec.h"
#include "coder/string_page.h"
#include <cstdlib>
#include <iostream>

using namespace pkv;

static void require(bool passed, const char* message) {
    if (!passed) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

int main() {
    const auto codec = default_record_codec();
    std::unique_ptr<record_view> view;
    record_header header;
    const std::string old = R"({"T":"S","E":0,"D":"old"})";
    require(codec->decode_stored(old, view) == codec_status::unsupported_version &&
        !view && codec->stored_header(old, header) == codec_status::unsupported_version,
        "legacy JSON record was accepted as a stored value or expiry");
    const std::string value("x\0\xff", 3);
    std::string bytes;
    const long long deadline = 4102444800123;
    require(string_page_codec::encode_inline(deadline, value, bytes, true) ==
        string_page_status::ok, "native String encoding failed");
    require(codec->stored_header(bytes, header) == codec_status::ok &&
        header.kind == value_kind::string && header.expires_at_ms == deadline,
        "native header lost the type or absolute deadline");
    require(codec->decode_stored(bytes, view) == codec_status::ok &&
        view && view->paged_payload(), "native root was rejected");
}
