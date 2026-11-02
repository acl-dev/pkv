#include <wiredtiger.h>
#include <cstdio>

// Refuse recovery so a successful reopen proves a clean native shutdown.
int main(int argc, char* argv[]) {
    if (argc != 2) return 2;
    WT_CONNECTION* connection = nullptr;
    const int opened = wiredtiger_open(argv[1], nullptr,
        "log=(enabled=true,recover=error)", &connection);
    if (opened) {
        std::fprintf(stderr, "Clean open failed: %d (%s)\n", opened,
            wiredtiger_strerror(opened));
        return 1;
    }
    const int closed = connection->close(connection, nullptr);
    if (closed) {
        std::fprintf(stderr, "Clean close failed: %d (%s)\n", closed,
            wiredtiger_strerror(closed));
        return 1;
    }
    return 0;
}
