#pragma once
#include <string>

namespace pkv {
bool parse_redis_double(const std::string& text, double& out);
bool parse_redis_score_endpoint(const std::string& text, double& out);
std::string format_redis_double(double value);
}
