#include "common/client_registry.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <vector>

using clock_type = std::chrono::steady_clock;
static volatile size_t observed = 0;
static double sample(const pkv::client_registry& registry, bool ordered) {
    const auto start = clock_type::now();
    const auto entries = registry.snapshot(ordered);
    const auto elapsed = std::chrono::duration<double>(
        clock_type::now() - start).count();
    observed += entries.size();
    return elapsed;
}
static double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}
int main() {
    std::cout << std::setprecision(10);
    std::cout << "{\"cpp\":" << __cplusplus << ",\"rounds\":21,\"results\":[";
    bool first = true;
    for (size_t count : {1024U, 4096U, 16384U, 65536U}) {
        pkv::client_registry registry;
        for (size_t i = 0; i < count; ++i)
            registry.add(static_cast<long long>(i + 1), 100);
        for (unsigned i = 0; i < 3; ++i) {
            sample(registry, true); sample(registry, false);
        }
        std::vector<double> ordered, unsorted;
        for (unsigned i = 0; i < 21; ++i) {
            if (i % 2) {
                unsorted.push_back(sample(registry, false));
                ordered.push_back(sample(registry, true));
            } else {
                ordered.push_back(sample(registry, true));
                unsorted.push_back(sample(registry, false));
            }
        }
        const double before = median(ordered), after = median(unsorted);
        if (!first) std::cout << ',';
        first = false;
        std::cout << "{\"connections\":" << count <<
            ",\"sorted_median_seconds\":" << before <<
            ",\"unsorted_median_seconds\":" << after <<
            ",\"speedup\":" << before / after << '}';
    }
    std::cout << "],\"observed_references\":" << observed << "}\n";
}
