#include "record_codec.h"

namespace pkv {
namespace {
class native_record_codec final : public record_codec {
public:
    codec_status decode(const std::string&,
        std::unique_ptr<record_view>& out) const override {
        out.reset();
        return codec_status::unsupported_version;
    }

    codec_status encode(const value_record&, std::string&) const override {
        return codec_status::unsupported_version;
    }
};
} // namespace

std::shared_ptr<const record_codec> default_record_codec() {
    static const auto codec = std::make_shared<const native_record_codec>();
    return codec;
}

} // namespace pkv
