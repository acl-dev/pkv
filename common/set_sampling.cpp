#include "set_sampling.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <thread>

namespace pkv {

std::mt19937_64& set_sampling_engine() {
    static std::atomic<unsigned long long> sequence{0};
    thread_local std::mt19937_64 generator([] {
        const auto now = static_cast<unsigned long long>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
        const auto thread = static_cast<unsigned long long>(std::hash<std::thread::id>()(std::this_thread::get_id()));
        const auto serial = ++sequence;
        unsigned entropy[4] = {};
        try {
            std::random_device source;
            for (auto& value : entropy) value = source();
        } catch (const std::exception&) {
            // Sampling remains available when OS entropy is unavailable. The
            // time, worker identity and process-local nonce still separate seeds.
        }
        std::seed_seq seed{entropy[0], entropy[1], entropy[2], entropy[3],
            static_cast<unsigned>(now), static_cast<unsigned>(now >> 32),
            static_cast<unsigned>(thread), static_cast<unsigned>(thread >> 32),
            static_cast<unsigned>(serial), static_cast<unsigned>(serial >> 32)};
        return std::mt19937_64(seed);
    }());
    return generator;
}

} // namespace pkv
