#include "bitfield.h"
#include "redis_integer.h"
#include <climits>

namespace pkv {
namespace {
uint64_t mask_for(unsigned bits) {
    return bits == 64 ? UINT64_MAX : (uint64_t(1) << bits) - 1;
}
} // namespace

bool parse_field_type(const std::string& text, bool& sign, unsigned& bits) {
    bits = 0;
    if (text.empty() || (text[0] != 'i' && text[0] != 'u')) return false;
    sign = text[0] == 'i';
    long long width;
    // Redis uses a C-string width here, while its offset parser is binary-safe.
    if (!parse_redis_integer(std::string(text.c_str() + 1), width) ||
        width < 1 || width > (sign ? 64 : 63)) return false;
    bits = static_cast<unsigned>(width);
    return true;
}

bool parse_field_offset(const std::string& text, unsigned bits, uint64_t& out) {
    out = 0;
    if (text.empty() || !bits || bits > 64) return false;
    const bool indexed = text[0] == '#';
    long long offset;
    if (!parse_redis_integer(indexed ? text.substr(1) : text, offset) ||
        offset < 0) return false;
    const uint64_t limit = (uint64_t(1) << 32) - 1;
    const uint64_t scale = indexed ? bits : 1;
    if (static_cast<uint64_t>(offset) > limit / scale) return false;
    out = static_cast<uint64_t>(offset) * scale;
    return true;
}

bool valid_bitfield(const bitfield_operation& operation) {
    if (!operation.bits || operation.bits > (operation.sign ? 64U : 63U) ||
        operation.offset >= (uint64_t(1) << 32)) return false;
    if (operation.overflow != field_overflow::wrap &&
        operation.overflow != field_overflow::saturate &&
        operation.overflow != field_overflow::fail) return false;
    return operation.command == field_command::get ||
        operation.command == field_command::set ||
        operation.command == field_command::increment;
}

long long field_decode(uint64_t raw, unsigned bits, bool sign) {
    const auto mask = mask_for(bits);
    raw &= mask;
    if (sign && (raw & (uint64_t(1) << (bits - 1))))
        return -1 - static_cast<long long>(mask - raw);
    return static_cast<long long>(raw);
}

long long field_read(const std::string& bytes, unsigned skip, unsigned bits,
    bool sign) {
    uint64_t raw = 0;
    for (unsigned i = 0; i < bits; ++i) {
        const auto index = skip + i;
        const auto byte = static_cast<unsigned char>(bytes[index / 8]);
        raw = (raw << 1) | ((byte >> (7 - index % 8)) & 1);
    }
    return field_decode(raw, bits, sign);
}

void field_write(std::string& bytes, unsigned skip, unsigned bits,
    uint64_t raw) {
    for (unsigned i = 0; i < bits; ++i) {
        const auto index = skip + i;
        const auto mask = 1U << (7 - index % 8);
        const auto byte = static_cast<unsigned char>(bytes[index / 8]);
        bytes[index / 8] = static_cast<char>(
            ((raw >> (bits - 1 - i)) & 1) ? byte | mask : byte & ~mask);
    }
}

bool field_update(const bitfield_operation& operation, long long old,
    long long& value) {
    const auto bits = operation.bits;
    const auto mask = mask_for(bits);
    const auto maximum = operation.sign ? (bits == 64 ? LLONG_MAX :
        (1LL << (bits - 1)) - 1) : static_cast<long long>(mask);
    const auto minimum = operation.sign ? -maximum - 1 : 0;
    const auto argument = operation.value;
    int direction = 0;
    uint64_t raw;
    if (operation.command == field_command::set) {
        raw = static_cast<uint64_t>(argument);
        if (operation.sign) {
            // Redis tests its wrapped signed max-delta before the lower
            // bound. Extreme negative SET arguments can therefore SAT to
            // the upper bound. Preserve that ordering without signed UB.
            const auto delta = static_cast<uint64_t>(maximum) - raw;
            const bool upper = argument > maximum || (bits != 64 &&
                delta > static_cast<uint64_t>(LLONG_MAX));
            direction = upper ? 1 : argument < minimum ? -1 : 0;
        }
        else direction = raw > mask ? 1 : 0;
    } else {
        raw = static_cast<uint64_t>(old) + static_cast<uint64_t>(argument);
        if (operation.sign) {
            if (argument > 0 && old > maximum - argument) direction = 1;
            if (argument < 0 && old < minimum - argument) direction = -1;
        } else if (argument < 0) {
            const auto magnitude = uint64_t(0) -
                static_cast<uint64_t>(argument);
            if (static_cast<uint64_t>(old) < magnitude) direction = -1;
        } else if (static_cast<uint64_t>(argument) >
            mask - static_cast<uint64_t>(old)) direction = 1;
    }
    if (direction && operation.overflow == field_overflow::fail) return false;
    if (direction && operation.overflow == field_overflow::saturate)
        value = direction < 0 ? minimum : maximum;
    else value = field_decode(raw, bits, operation.sign);
    return true;
}

} // namespace pkv
