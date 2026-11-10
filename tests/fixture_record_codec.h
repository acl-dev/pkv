#pragma once
#include "coder/record_codec.h"

namespace pkv {

// Test-only format used to exercise the injected codec and non-page DB APIs.
class fixture_record_codec final : public record_codec {
public:
    codec_status decode(const std::string&, std::unique_ptr<record_view>&) const override;
    codec_status encode(const value_record&, std::string&) const override;
};

} // namespace pkv
