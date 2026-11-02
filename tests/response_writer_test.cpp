#include "common/response_writer.h"
#include <cstdlib>
#include <iostream>
#include <limits>

static void require(bool ok, const char* reason) {
    if (!ok) { std::cerr << reason << '\n'; std::exit(1); }
}
int main() {
    const size_t chunk = 16ULL * 1024 * 1024;
    for (size_t length : {size_t(0), size_t(1), chunk - 1, chunk, chunk + 1,
        size_t(INT_MAX), size_t(INT_MAX) + 1, size_t(INT_MAX) + 100}) {
        size_t expected = 0, written = 999, calls = 0;
        const bool ok = pkv::response_write(length, written,
            [&](size_t offset, size_t count) {
                require(offset == expected && count > 0 && count <= chunk &&
                    count <= INT_MAX && count <= length - offset,
                    "invalid chunk or offset");
                expected += count;
                ++calls;
                return count;
            });
        require(ok && written == length && expected == length &&
            calls == (length + chunk - 1) / chunk, "truncated response");
    }
    size_t written = 0, calls = 0;
    const bool partial = pkv::response_write(chunk * 3, written,
        [&](size_t, size_t count) {
            return ++calls == 2 ? count / 2 : count;
        });
    require(!partial && calls == 2 && written == chunk + chunk / 2,
        "partial write accounting");
    const bool closed = pkv::response_write(std::numeric_limits<size_t>::max(),
        written, [](size_t, size_t) { return size_t(0); });
    require(!closed && !written, "closed stream continued writing");
    const bool invalid = pkv::response_write(chunk, written,
        [](size_t, size_t count) { return count + 1; });
    require(!invalid && !written, "writer over-report accepted");
    std::cout << "Bounded response writes and signed-int boundary passed\n";
}
