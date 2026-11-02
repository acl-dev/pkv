#pragma once
#include "record_codec.h"

namespace pkv {

// v0: unversioned {T,E,D}, E in seconds. v1: {V:1,T,E,P,D}, P in
// milliseconds and E its ceiling in seconds for older readers. v0 writes are
// available for explicit export and reject deadlines that lose precision.
class json_record_codec : public record_codec {
public:
    explicit json_record_codec(unsigned write_version = 1) : version_(write_version) {}
    codec_status decode(const std::string& bytes, std::unique_ptr<record_view>& out) const override;
    codec_status encode(const value_record& record, std::string& out) const override;
private:
    unsigned version_;
};

} // namespace pkv
