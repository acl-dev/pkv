#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace pkv {
namespace hll {
static const size_t registers_size = 16384;
static const size_t header_size = 16;
static const size_t dense_size = 12304;
static const size_t sparse_limit = 3000;
static const size_t maximum_size = header_size + 2 * registers_size;
using registers = std::array<uint8_t, registers_size>;
bool header(const std::string& prefix, uint64_t length);
bool cached(const std::string& value, uint64_t& count);
void cache(std::string& value, uint64_t count);
void invalidate(std::string& value);
std::string empty();
uint64_t hash(const std::string& value);
uint8_t get(const std::string& value, size_t index);
void put(std::string& value, size_t index, uint8_t count);
bool merge(const std::string& value, registers& into);
bool dense(std::string& value);
bool set(std::string& value, size_t index, uint8_t count, bool& changed);
bool add(std::string& value, const std::string& element, bool& changed);
uint64_t count(const registers& values);
struct run { size_t length = 0, bytes = 0; uint8_t value = 0; };
bool decode_run(const std::string& value, size_t offset, run& out);
void encode_run(std::string& value, size_t length, uint8_t count);
} // namespace hll
} // namespace pkv
