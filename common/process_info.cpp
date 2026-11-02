#include "process_info.h"
#include <fstream>
#include <limits>
#include <stdexcept>
#ifndef _WIN32
#include <sys/resource.h>
#include <sys/utsname.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <mach/mach.h>
#include <malloc/malloc.h>
#elif defined(__GLIBC__)
#include <malloc.h>
#endif
#ifdef HAS_JEMALLOC
#include <jemalloc/jemalloc.h>
#endif
#ifndef __has_feature
#define __has_feature(x) 0
#endif
#if __has_feature(address_sanitizer) || defined(__SANITIZE_ADDRESS__)
#include <sanitizer/allocator_interface.h>
#endif

namespace pkv {
process_identity::process_identity() { initialize(); }
void process_identity::initialize() {
    std::string bytes(20, '\0'), id(40, '\0');
    std::ifstream source("/dev/urandom", std::ios::binary);
    if (!source.read(&bytes[0], bytes.size()))
        throw std::runtime_error("process identity entropy unavailable");
    const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < bytes.size(); ++i) {
        const auto value = static_cast<unsigned char>(bytes[i]);
        id[2 * i] = hex[value >> 4]; id[2 * i + 1] = hex[value & 15];
    }
    std::string os;
#ifndef _WIN32
    struct utsname details{};
    if (!::uname(&details)) os = std::string(details.sysname) + ' ' +
        details.release + ' ' + details.machine;
    pid_ = ::getpid();
#endif
    run_id_.swap(id); os_.swap(os);
    start_ = std::chrono::steady_clock::now();
}
uint64_t process_identity::uptime() const {
    return static_cast<uint64_t>(std::chrono::duration_cast<
        std::chrono::seconds>(std::chrono::steady_clock::now() - start_).
        count());
}
namespace {
void heap(process_sample& out) {
#if __has_feature(address_sanitizer) || defined(__SANITIZE_ADDRESS__)
    out.heap = __sanitizer_get_current_allocated_bytes();
    out.allocator = "asan"; out.heap_known = true;
#elif defined(HAS_JEMALLOC)
    uint64_t epoch = 1; size_t length = sizeof(epoch);
    if (mallctl("epoch", &epoch, &length, &epoch, sizeof(epoch))) return;
    size_t allocated = 0; length = sizeof(allocated);
    if (mallctl("stats.allocated", &allocated, &length, nullptr, 0)) return;
    out.heap = allocated; out.allocator = "jemalloc"; out.heap_known = true;
#elif defined(__APPLE__)
    malloc_statistics_t statistics{};
    malloc_zone_statistics(nullptr, &statistics);
    out.heap = statistics.size_in_use;
    out.allocator = "libc"; out.heap_known = true;
#elif defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 33)
    const auto statistics = mallinfo2();
    if (statistics.hblkhd > std::numeric_limits<uint64_t>::max() -
        statistics.uordblks) return;
    out.heap = statistics.uordblks + statistics.hblkhd;
    out.allocator = "libc"; out.heap_known = true;
#endif
#endif
}
void resident(process_sample& out) {
#ifdef __APPLE__
    mach_task_basic_info_data_t details{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
        reinterpret_cast<task_info_t>(&details), &count) == KERN_SUCCESS) {
        out.rss = details.resident_size; out.rss_known = true;
    }
#elif defined(__linux__)
    std::ifstream input("/proc/self/statm");
    uint64_t total, pages;
    const auto size = ::sysconf(_SC_PAGESIZE);
    if (size > 0 && input >> total >> pages && pages <=
        std::numeric_limits<uint64_t>::max() / static_cast<uint64_t>(size)) {
        out.rss = pages * static_cast<uint64_t>(size); out.rss_known = true;
    }
#else
    (void) out;
#endif
}
} // namespace
process_sample sample_process() {
    process_sample out; heap(out); resident(out);
#ifndef _WIN32
    struct rusage own{}, child{};
    if (!getrusage(RUSAGE_SELF, &own) && !getrusage(RUSAGE_CHILDREN, &child)) {
        out.user = own.ru_utime.tv_sec + own.ru_utime.tv_usec / 1000000.0;
        out.system = own.ru_stime.tv_sec + own.ru_stime.tv_usec / 1000000.0;
        out.child_user = child.ru_utime.tv_sec + child.ru_utime.tv_usec / 1e6;
        out.child_system = child.ru_stime.tv_sec + child.ru_stime.tv_usec / 1e6;
        out.cpu_known = true;
    }
#endif
    return out;
}
} // namespace pkv
