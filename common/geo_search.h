#pragma once
#include "redis_geo.h"
#include "zset_value.h"
#include <array>
#include <functional>
#include <vector>

namespace pkv {
namespace geo {
struct search_source {
    bool exists;
    std::function<bool(const std::string&, zset_score&)> get;
};
struct shape {
    point center;
    double conversion = 1, radius = 0, width = 0, height = 0;
    bool box = false;
};
struct search_options {
    shape area;
    int order = 0; // -1 descending, 0 encounter order, 1 ascending.
    long long count = 0;
    bool any = false, storedist = false;
    bool withdist = false, withhash = false, withcoord = false;
};
struct match {
    std::string member;
    double score = 0, distance = 0;
    point position;
};
struct score_range { double lower = 0, upper = 0; bool enabled = false; };
std::array<score_range, 9> search_ranges(const shape& area);
bool contains(const shape& area, const point& position, double& meters);
} // namespace geo
} // namespace pkv
