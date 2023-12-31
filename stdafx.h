// stdafx.h : 标准系统包含文件的包含文件，
// 或是常用但不常更改的项目特定的包含文件
//

#pragma once


//#include <iostream>
//#include <tchar.h>
#include <memory>
#include <utility>
#include <thread>
#include <mutex>
#include <unordered_map>

// TODO: 在此处引用程序要求的附加头文件

#include <acl-lib/acl/lib_acl.h>
#include <acl-lib/acl_cpp/lib_acl.hpp>
#include <acl-lib/fiber/lib_fiber.h>
#include <acl-lib/fiber/lib_fiber.hpp>
#include <acl-lib/fiber/go_fiber.hpp>

#include "c++_patch.h"

#ifdef	WIN32
#define	snprintf _snprintf
#endif

#undef logger
#undef logger_warn
#undef logger_error
#undef logger_fatal
#undef logger_debug

#if defined(_WIN32) || defined(_WIN64)

# if _MSC_VER >= 1500
#  define logger(fmt, ...)  \
	acl::log::msg4(__FILE__, __LINE__, __FUNCTION__, fmt, __VA_ARGS__)
#  define logger_warn(fmt, ...)  \
	acl::log::warn4(__FILE__, __LINE__, __FUNCTION__, fmt, __VA_ARGS__)
#  define logger_error(fmt, ...)  \
	acl::log::error4(__FILE__, __LINE__, __FUNCTION__, fmt, __VA_ARGS__)
#  define logger_fatal(fmt, ...)  \
	acl::log::fatal4(__FILE__, __LINE__, __FUNCTION__, fmt, __VA_ARGS__)
#  define logger_debug(section, level, fmt, ...)  \
	acl::log::msg6(section, level, __FILE__, __LINE__, __FUNCTION__, fmt, __VA_ARGS__)
# else
#  define logger	acl::log::msg1
#  define logger_warn	acl::log::warn1
#  define logger_error	acl::log::error1
#  define logger_fatal	acl::log::fatal1
#  define logger_debug	acl::log::msg3
# endif
#else
# define logger(fmt, args...)  \
	acl::log::msg4(__FILE__, __LINE__, __FUNCTION__, fmt, ##args)
# define logger_warn(fmt, args...)  \
	acl::log::warn4(__FILE__, __LINE__, __FUNCTION__, fmt, ##args)
# define logger_error(fmt, args...)  \
	acl::log::error4(__FILE__, __LINE__, __FUNCTION__, fmt, ##args)
# define logger_fatal(fmt, args...)  \
	acl::log::fatal4(__FILE__, __LINE__, __FUNCTION__, fmt, ##args)
# define logger_debug(section, level, fmt, args...)  \
	acl::log::msg6(section, level, __FILE__, __LINE__, __FUNCTION__, fmt, ##args)
#endif // !_WIN32 && !_WIN64

extern char *var_cfg_service_addr;
extern acl::master_str_tbl var_conf_str_tab[];

extern int var_cfg_disable_serialize;
extern int var_cfg_disable_save;
extern int var_cfg_cluster_mode;
extern acl::master_bool_tbl var_conf_bool_tab[];

extern int var_cfg_redis_max_slots;
extern int var_cfg_slave_client_stack;
extern int var_cfg_slave_messages_flush;
extern acl::master_int_tbl var_conf_int_tab[];
extern acl::master_int64_tbl var_conf_int64_tab[];

#define SERVICE_RPC_PORT_ADD    10000

#ifndef HAS_ROCKSDB
# define HAS_ROCKSDB
#endif

#ifndef HAS_WT
# define HAS_WT
#endif

//#define HAS_VECTOR
