#pragma once
#include <cstdint>
#include <string>

namespace pkv {

enum class field_command { get, set, increment };
enum class field_overflow { wrap, saturate, fail };
struct bitfield_operation {
    field_command command = field_command::get;
    field_overflow overflow = field_overflow::wrap;
    bool sign = false;
    unsigned bits = 8;
    uint64_t offset = 0;
    long long value = 0;
};
struct bitfield_reply {
    bool null = false;
    long long value = 0;
};
bool parse_field_type(const std::string& text, bool& sign, unsigned& bits);
bool parse_field_offset(const std::string& text, unsigned bits, uint64_t& out);
bool valid_bitfield(const bitfield_operation& operation);
long long field_decode(uint64_t raw, unsigned bits, bool sign);
long long field_read(const std::string& bytes, unsigned skip, unsigned bits,
    bool sign);
void field_write(std::string& bytes, unsigned skip, unsigned bits,
    uint64_t raw);
// False means an OVERFLOW FAIL suboperation, not a failed transaction.
bool field_update(const bitfield_operation& operation, long long old,
    long long& value);

} // namespace pkv
