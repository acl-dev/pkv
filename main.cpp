#include "stdafx.h"
#include <csignal>
#include "common/shutdown_signals.h"
#include "test/test.h"
#include "master_service.h"

#ifdef __APPLE__
#include <sys/resource.h>
#include <sys/sysctl.h>

static void bound_fiber_fd_tables() {
    int ceiling = 0;
    size_t size = sizeof(ceiling);
    struct rlimit limits;
    if (sysctlbyname("kern.maxfilesperproc", &ceiling, &size, nullptr, 0) != 0 ||
        size != sizeof(ceiling) || ceiling <= 0 ||
        getrlimit(RLIMIT_NOFILE, &limits) != 0)
        return;
    const auto bound = static_cast<rlim_t>(ceiling);
    if (limits.rlim_max <= bound) return;
    // ACL raises the soft limit to the hard limit and sizes its event tables
    // from that value. Darwin's descriptor allocator already enforces bound.
    limits.rlim_max = bound;
    if (limits.rlim_cur > bound) limits.rlim_cur = bound;
    if (setrlimit(RLIMIT_NOFILE, &limits) != 0)
        std::perror("Cannot bound ACL file descriptor tables");
}
#endif

#ifdef _WIN32
static void on_sigint(int) {
    master_service& ms = master_service::get_instance();
    logger("---Begin to close db---");
    ms.close_db();
    logger("---Close db ok---");
    _exit(0);
}
#endif

int main(int argc, char *argv[]) {
    const bool serving = argc < 2 || strcasecmp(argv[1], "test") != 0;
    if (serving && (!pkv::block_shutdown_signals() ||
        !pkv::start_shutdown_signals())) {
        std::fprintf(stderr, "Cannot prepare shutdown signals\n"); return 1;
    }
    acl::acl_cpp_init();

    if (argc >= 2 && strcasecmp(argv[1], "test") == 0) {
        pkv::test test;
        test.run(argc, argv);
        return 0;
    }

#ifdef __APPLE__
    bound_fiber_fd_tables();
    // Complete ACL's kqueue hook lookup before concurrent schedulers start.
    std::thread runtime([] {
        go[] { acl::fiber::delay(1); };
        acl::fiber::schedule();
    });
    runtime.join();
#endif

    master_service& ms = master_service::get_instance();

    // 设置配置参数表
    ms.set_cfg_int(var_conf_int_tab);
    ms.set_cfg_int64(var_conf_int64_tab);
    ms.set_cfg_str(var_conf_str_tab);
    ms.set_cfg_bool(var_conf_bool_tab);

    if (argc == 1 || (argc >= 2 && strcasecmp(argv[1], "alone") == 0)) {
#ifdef _WIN32
        signal(SIGINT, on_sigint);
#endif

        // 日志输出至标准输出
        acl::log::stdout_open(true);
        // 禁止生成 acl_master.log 日志
        acl::master_log_enable(false);

#ifndef _WIN32
        // ACL's C runtime infers daemon mode from stdin and MASTER_SERVICE,
        // independently of master_fiber::run_alone(). Honor the explicit mode
        // even with redirected stdin and inherited supervisor descriptors.
        if (setenv("MASTER_SERVICE", "ALONE", 1) != 0) {
            std::perror("Cannot select standalone service mode"); return 1;
        }
#endif
        const char* addr = nullptr;
        //printf("listen: %s\r\n", addr);
        ms.run_alone(addr, argc >= 3 ? argv[2] : nullptr);
    } else {
#if defined(_WIN32) || defined(_WIN64)
        const char* addr = "|8887";

		acl::log::stdout_open(true);
		printf("listen on: %s\r\n", addrs);
		ms.run_alone(addr, argc >= 3 ? argv[2] : NULL);
#else
        ms.run_daemon(argc, argv);
#endif
    }

    return 0;
}
