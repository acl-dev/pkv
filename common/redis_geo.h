#pragma once
#include <cstdint>
#include <string>

namespace pkv {
namespace geo {
struct point { double longitude = 0, latitude = 0; };
bool valid(const point& value);
std::string coordinate_error(const point& value);
// Requires valid(value); inclusive upper bounds retain their edge bits.
uint64_t encode(const point& value);
// Defined saturating conversion also handles arbitrary ordinary ZSet scores.
point decode(double score);
std::string hash(const point& value);
// Coordinates must be finite and inside valid() bounds.
double distance(const point& first, const point& second);
std::string distance_text(double meters);
double unit(const std::string& name);
} // namespace geo
} // namespace pkv
