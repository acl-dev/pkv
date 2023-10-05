#include "stdafx.h"
#include "cluster/cluster_manager.h"
#include "coder/redis_ocache.h"
#include "coder/redis_coder.h"
#include "action/redis_handler.h"
#include "action/redis_service.h"
#include "db/db_watcher.h"
#include "cluster/cluster_service.h"
#include "slave/slave_watcher.h"
#include "common/message_box.h"
#include "master_service.h"

static char *var_cfg_dbpath;
static char *var_cfg_dbtype;
char *var_cfg_service_addr;
static char *var_cfg_cluster_path;

acl::master_str_tbl var_conf_str_tab[] = {
    { "dbpath",         "./dbpath",         &var_cfg_dbpath         },
    { "dbtype",         "rdb",              &var_cfg_dbtype         },
    { "service",        "127.0.0.1:19001",  &var_cfg_service_addr   },
    { "cluster_path",   "",                 &var_cfg_cluster_path   },

    { nullptr,          nullptr,            nullptr                 },
};

int var_cfg_disable_serialize;
int var_cfg_disable_save;
int var_cfg_cluster_mode;

acl::master_bool_tbl var_conf_bool_tab[] = {
    { "disable_serialize",  0 , &var_cfg_disable_serialize  },
    { "disable_save",       0,  &var_cfg_disable_save       },
    { "cluster_mode",       0,  &var_cfg_cluster_mode       },

    { nullptr,              0,  nullptr                     },
};

static int  var_cfg_io_timeout;
static int  var_cfg_buf_size;
static int  var_cfg_ocache_max;
static int  var_cfg_db_count;
int var_cfg_redis_max_slots;
int var_cfg_slave_messages_flush;
int var_cfg_slave_client_stack;

acl::master_int_tbl var_conf_int_tab[] = {
    { "io_timeout",           120,     &var_cfg_io_timeout,           0, 0 },
    { "buf_size",             8192,    &var_cfg_buf_size,             0, 0 },
    { "ocache_max",           10000,   &var_cfg_ocache_max,           0, 0 },
    { "redis_max_slots",      16384,   &var_cfg_redis_max_slots,      0, 0 },
    { "db_count",             256,     &var_cfg_db_count,             0, 0 },
    { "slave_messages_flush", 500,     &var_cfg_slave_messages_flush, 0, 0 },
    { "slave_client_stack",   1024000, &var_cfg_slave_client_stack,   0, 0 },

    { nullptr,                0 ,    nullptr ,                      0, 0 },
};

acl::master_int64_tbl var_conf_int64_tab[] = {
    { nullptr, 0 , nullptr , 0, 0 }
};


//////////////////////////////////////////////////////////////////////////

using namespace pkv;

master_service::master_service()
: manager_(cluster_manager::get_instance())
{
    service_ = new pkv::redis_service;
    assert(service_);
    watchers_ = new db_watchers;
    assert(watchers_);
}

master_service::~master_service() {
    delete service_;
    delete watchers_;
}

void master_service::on_accept(acl::socket_stream& conn) {
    //conn.set_rw_timeout(var_cfg_io_timeout);
    //logger(">>>accept connection: %d", conn.sock_handle());

    run(conn, var_cfg_buf_size);

    //logger("Disconnect from peer, fd=%d", conn.sock_handle());
}

// The cache object for each thread which has been created in thread_on_init.
static __thread redis_ocache* ocache;

void master_service::run(acl::socket_stream& conn, size_t size) {
    pkv::redis_coder parser(*ocache);
    pkv::redis_handler handler(*service_, db_, parser, conn);
    char buf[size];

    while(true) {
        int ret = conn.read(buf, sizeof(buf) - 1, false);
        if (ret <= 0) {
            break;
        }

        buf[ret] = 0;

        //printf("%s, len=%zd", buf, strlen(buf)); fflush(stdout);

        auto len = (size_t) ret;
        const char* data = parser.update(buf, len);
        auto obj = parser.get_curr();
        assert(obj);
        if (obj->failed()) {
            break;
        }

        if (*data != 0 || len != 0) {
            abort();
        }

        if (!handler.handle()) {
            break;
        }

        parser.clear();
    }
}

void master_service::proc_pre_jail() {
    logger(">>>proc_pre_jail<<<");
}

void master_service::proc_on_listen(acl::server_socket& ss) {
    logger(">>>listen %s ok<<<", ss.get_addr());
}

void master_service::proc_on_init() {
    if (strcasecmp(var_cfg_dbtype, "rdb") == 0) {
        db_ = db::create_rdb();
    } else if (strcasecmp(var_cfg_dbtype, "wdb") == 0) {
        db_ = db::create_wdb();
    } else if (strcasecmp(var_cfg_dbtype, "mdb") == 0) {
        db_ = db::create_mdb(var_cfg_db_count);
    } else if (strcasecmp(var_cfg_dbtype, "mdb_htable") == 0) {
        db_ = db::create_mdb_htable(var_cfg_db_count);
    } else if (strcasecmp(var_cfg_dbtype, "mdb_avl") == 0) {
        db_ = db::create_mdb_avl();
    } else if (strcasecmp(var_cfg_dbtype, "mdb_tbb") == 0) {
        db_ = db::create_mdb_tbb(var_cfg_db_count);
    } else {
        logger_error("unknown dbtype=%s", var_cfg_dbtype);
        exit(1);
    }

    if (!db_->open(var_cfg_dbpath, watchers_)) {
        logger_error("open db(%s) error: %s", var_cfg_dbpath, acl::last_serror());
        exit(1);
    }

    acl::string buf(var_cfg_service_addr);
    auto& tokens = buf.split2(":|");
    if (tokens.size() != 2) {
        logger_error("Invalid service=%s", var_cfg_service_addr);
        exit(1);
    }

    char* end;
    int service_port = (int) std::strtol(tokens[1].c_str(), &end, 10);
    if (*end != 0) {
        logger_error("Invalid port=%s, service addr=%s", end, var_cfg_service_addr);
    }
    if (service_port <= 0 || service_port >= 60000) {
        logger_error("Invalid port=%d, service addr=%s",
            service_port, var_cfg_service_addr);
    }

    int rpc_port = service_port + SERVICE_RPC_PORT_ADD;

    manager_.init(var_cfg_redis_max_slots, var_cfg_cluster_mode,
                  var_cfg_cluster_path, tokens[0].c_str(),
                  service_port, rpc_port, db_);
}

void master_service::close_db() {
    if (db_) {
        db_ = nullptr;
    }
}

void master_service::proc_on_exit() {
    logger(">>>proc_on_exit<<<");
    close_db();
}

bool master_service::proc_on_sighup(acl::string&) {
    logger(">>>proc_on_sighup<<<");
    return true;
}

static __thread message_box* box;
std::once_flag watch_once;

// One watcher for wartching the changing of db.
pkv::slave_watcher *watcher;

void master_service::thread_on_init() {
    // Prepare the cache object first for each thread.
    ocache = new redis_ocache(var_cfg_ocache_max);
    assert(ocache);

    for (int i = 0; i < var_cfg_ocache_max; i++) {
        auto o = new pkv::redis_object(*ocache);
        ocache->put(o);
    }

    std::call_once(watch_once, [this] { start_watcher(); });

    assert(watcher);

    // Create one box for the current thread and bind it in the thread.
    box = new message_box;
    slave_watcher::set_box(*watcher, box);
}

void master_service::start_watcher() {
#undef __FUNCTION__
#define __FUNCTION__ "start_watcher"
    watcher = new slave_watcher;
    assert(watcher);
    watchers_->add_watcher(watcher);

    // Start one thread for acceptint connection from other nodes.
    std::thread thr1([this] {
        // Start one server fiber for every thread to handle the process
        // betwwen different nodes.
        go[this] {
            pkv::cluster_service service(*watcher);
            acl::string addr;
            addr.format("%s:%d", manager_.get_service_ip(), manager_.get_rpc_port());
            // Bind the local address to accept connection from other nodes.
            if (!service.bind(addr)) {
                logger_error("Bind %s error %s", addr.c_str(), acl::last_serror());
            } else {
                logger("Bind rpc addr=%s ok", addr.c_str());
                service.run();
            }
        };
    
        acl::fiber::schedule();
    });
    thr1.detach();

    // Start one thread to wait db messages and forward them to the clients.
    std::thread thr2([] {
        // Start one watcher fiber to wait messages from box.
        go[] {
            watcher->run();
        };

        acl::fiber::schedule();
    });
    thr2.detach();
}
