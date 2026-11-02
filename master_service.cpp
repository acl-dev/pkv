#include "stdafx.h"
#include "common/redis_integer.h"
#include "common/shutdown_signals.h"
#include <dirent.h>
#include <cerrno>
#include <stdexcept>
#include "cluster/cluster_manager.h"
#include "coder/redis_ocache.h"
#include "coder/redis_coder.h"
#include "action/redis_handler.h"
#include "action/redis_service.h"
#include "db/db_watcher.h"
#include "db/database_pool.h"
#include "db/move_domain.h"
#include "db/move_recovery.h"
#include "db/expiry_service.h"
#include "cluster/cluster_service.h"
#include "cluster/cluster_client.h"
#include "slave/slave_watcher.h"
#include "common/message_box.h"
#include "master_service.h"

static char *var_cfg_dbpath;
static char *var_cfg_dbtype;
char *var_cfg_service_addr;
static char *var_cfg_cluster_path;
static char *var_cfg_replica_rpc_source;
static char *var_cfg_requirepass;
static char *var_cfg_masterauth;
static char *var_cfg_aclfile;
static char *var_cfg_move_journal_path;

acl::master_str_tbl var_conf_str_tab[] = {
    { "dbpath",         "./dbpath",         &var_cfg_dbpath         },
    { "dbtype",         "rdb",              &var_cfg_dbtype         },
    { "service",        "127.0.0.1:19001",  &var_cfg_service_addr   },
    { "replica_rpc_source", "",             &var_cfg_replica_rpc_source },
    { "requirepass",    "",                 &var_cfg_requirepass    },
    { "aclfile",        "",                 &var_cfg_aclfile        },
    { "move_journal_path", "auto", &var_cfg_move_journal_path },
    { "masterauth",     "",                 &var_cfg_masterauth     },
    { "cluster_path",   "",                 &var_cfg_cluster_path   },

    { nullptr,          nullptr,            nullptr                 },
};

int var_cfg_disable_serialize;
int var_cfg_disable_save;
int var_cfg_cluster_mode;
static int var_cfg_active_expire;
static int var_cfg_replica_read_only;

acl::master_bool_tbl var_conf_bool_tab[] = {
    { "disable_serialize",  0 , &var_cfg_disable_serialize  },
    { "disable_save",       0,  &var_cfg_disable_save       },
    { "active_expire",      1,  &var_cfg_active_expire      },
    { "replica_read_only", 1, &var_cfg_replica_read_only },
    { "cluster_mode",       0,  &var_cfg_cluster_mode       },

    { nullptr,              0,  nullptr                     },
};

static int  var_cfg_io_timeout;
static int  var_cfg_buf_size;
static int  var_cfg_ocache_max;
static int  var_cfg_db_count;
static int var_cfg_databases;
static int var_cfg_acllog_max_len;
static int var_cfg_expire_batch;
static int var_cfg_expire_interval;
int var_cfg_redis_max_slots;
int var_cfg_slave_messages_flush;
int var_cfg_slave_client_stack;
int var_cfg_replica_output_buffer_limit;
int var_cfg_replica_publish_buffer_limit;
int var_cfg_replica_pending_messages;

acl::master_int_tbl var_conf_int_tab[] = {
    { "io_timeout",           120,     &var_cfg_io_timeout,           0, 0 },
    { "buf_size",             8192,    &var_cfg_buf_size,             0, 0 },
    { "ocache_max",           10000,   &var_cfg_ocache_max,           0, 0 },
    { "redis_max_slots",      16384,   &var_cfg_redis_max_slots,      0, 0 },
    { "active_expire_batch", 64, &var_cfg_expire_batch, 1, 1024 },
    { "active_expire_interval", 100, &var_cfg_expire_interval, 1, 1000 },
    { "databases",            16,      &var_cfg_databases,            1, 65536
        },
    { "acllog-max-len", 128, &var_cfg_acllog_max_len, 0, 2147483647 },
    { "db_count",             256,     &var_cfg_db_count,             0, 0 },
    { "replica_output_buffer_limit", 67108864,
        &var_cfg_replica_output_buffer_limit, 1, 2147483647 },
    { "replica_publish_buffer_limit", 67108864,
        &var_cfg_replica_publish_buffer_limit, 1, 2147483647 },
    { "replica_pending_messages", 65536, &var_cfg_replica_pending_messages, 1,
        2147483647 },
    { "slave_messages_flush", 100,     &var_cfg_slave_messages_flush, 0, 0 },
    { "slave_client_stack",   1024000, &var_cfg_slave_client_stack,   0, 0 },

    { nullptr,                0 ,    nullptr ,                      0, 0 },
};

acl::master_int64_tbl var_conf_int64_tab[] = {
    { nullptr, 0 , nullptr , 0, 0 }
};


//////////////////////////////////////////////////////////////////////////

using namespace pkv;

master_service& master_service::get_instance() {
    static master_service instance;
    return instance;
}

master_service::master_service()
: manager_(cluster_manager::get_instance())
{
    service_ = new pkv::redis_service;
    assert(service_);
    watchers_ = new db_watchers;
    assert(watchers_);
}

master_service::~master_service() {
    expiry_.reset();
    delete service_;
    delete watchers_;
}

void master_service::on_accept(acl::socket_stream& conn) {
    //conn.set_rw_timeout(var_cfg_io_timeout);
    //logger(">>>accept connection: %d", conn.sock_handle());

    try {
        auto& statistics = service_->statistics().worker(
            pkv::client_worker_id());
        statistics.connection();
        run(conn, var_cfg_buf_size, statistics);
    } catch (const std::bad_alloc&) {
        logger_error("Client allocation failed; closing connection");
    }

    //logger("Disconnect from peer, fd=%d", conn.sock_handle());
}

// The cache object for each thread which has been created in thread_on_init.
static __thread redis_ocache* ocache;

void master_service::run(acl::socket_stream& conn, size_t size,
    pkv::worker_statistics& statistics) {
    pkv::redis_coder parser(*ocache);
    pkv::redis_handler handler(*service_, databases_, parser, conn,
        &statistics);
    std::vector<char> buf(size < 2 ? 2 : size);

    while(true) {
        int ret = handler.read(buf.data(), buf.size() - 1);
        if (ret <= 0) {
            break;
        }

        buf[ret] = 0;

        //printf("%s, len=%zd", buf, strlen(buf)); fflush(stdout);

        auto len = (size_t) ret;
        handler.receive(buf.data(), len, buf.capacity());
        auto obj = parser.get_curr();
        assert(obj);
        if (obj->failed()) {
            break;
        }

        if (len != 0) {
            break;
        }

        if (!handler.handle()) {
            break;
        }

        handler.clear_input();
    }
}

void master_service::proc_pre_jail() {
    logger(">>>proc_pre_jail<<<");
}

void master_service::proc_on_listen(acl::server_socket& ss) {
    logger(">>>listen %s ok<<<", ss.get_addr());
}

std::vector<std::string> master_service::database_roots() const {
    if (move_domain_ && move_domain_->enabled()) return move_domain_->roots();
    std::vector<std::string> bases;
    acl::string paths(var_cfg_dbpath);
    for (const auto& base : paths.split2(";, \t")) bases.emplace_back(base);
    return bases;
}

pkv::shared_db master_service::create_database(size_t index) {
    pkv::shared_db created;
    if (strcasecmp(var_cfg_dbtype, "rdb") == 0) {
        created = db::create_rdb();
    } else if (strcasecmp(var_cfg_dbtype, "wdb") == 0) {
        created = db::create_wdb();
    } else if (strcasecmp(var_cfg_dbtype, "mdb") == 0) {
        created = db::create_mdb(var_cfg_db_count);
    } else if (strcasecmp(var_cfg_dbtype, "mdb_htable") == 0) {
        created = db::create_mdb_htable(var_cfg_db_count);
    } else if (strcasecmp(var_cfg_dbtype, "mdb_avl") == 0) {
        created = db::create_mdb_avl();
    } else if (strcasecmp(var_cfg_dbtype, "mdb_tbb") == 0) {
        created = db::create_mdb_tbb(var_cfg_db_count);
    } else {
        logger_error("unknown dbtype=%s", var_cfg_dbtype);
        return nullptr;
    }

    if (!created) {
        logger_error("Requested backend is unavailable in this build");
        return nullptr;
    }
    const auto bases = database_roots();
    std::vector<std::string> selected_roots;
    std::string selected_paths;
    for (const auto& base : bases) {
        auto path = base;
        if (index) path += "/logical-db-" + std::to_string(index);
        if (!selected_paths.empty()) selected_paths += ";";
        selected_paths += path;
        selected_roots.emplace_back(std::move(path));
    }
    std::shared_ptr<db_watchers> observers;
    if (index) {
        if (!watcher_) {
            return nullptr;
        }
        observers = std::make_shared<db_watchers>(static_cast<unsigned>(index));
        observers->add_watcher(watcher_);
    }
    const bool native = !strcasecmp(var_cfg_dbtype, "rdb") ||
        !strcasecmp(var_cfg_dbtype, "wdb");
    bool opened;
    if (native) {
        opened = index ? created->open_observed_paths(selected_roots,
            observers) : created->open_paths(selected_roots, watchers_);
    } else {
        opened = index ? created->open_observed(selected_paths.c_str(),
            observers) : created->open(selected_paths.c_str(), watchers_);
    }
    if (!opened) {
        logger_error("open db(%s) error: %s", selected_paths.c_str(),
            acl::last_serror());
        return nullptr;
    }

    return created;
}

std::vector<size_t> master_service::existing_databases() const {
    std::vector<size_t> found;
    if (strcasecmp(var_cfg_dbtype, "rdb") && strcasecmp(var_cfg_dbtype, "wdb"))
        return found;
    for (const auto& base : database_roots()) {
        DIR* raw = ::opendir(base.c_str());
        if (!raw) throw std::runtime_error("database directory scan failed");
        std::unique_ptr<DIR, int (*)(DIR*)> directory(raw, ::closedir);
        for (;;) {
            errno = 0;
            const auto* item = ::readdir(directory.get());
            if (!item) {
                if (errno) throw std::runtime_error("database scan failed");
                break;
            }
            const std::string name(item->d_name), prefix("logical-db-");
            if (name.compare(0, prefix.size(), prefix)) continue;
            long long index;
            if (pkv::parse_redis_integer(name.substr(prefix.size()), index) &&
                index > 0 && index < var_cfg_databases)
                found.push_back(static_cast<size_t>(index));
        }
    }
    return found;
}

void master_service::proc_on_init() {
    service_->identity().initialize();
    if (var_cfg_acllog_max_len < 0) {
        logger_error("acllog-max-len must not be negative"); exit(1);
    }
    service_->audit().limit(static_cast<size_t>(var_cfg_acllog_max_len));
    service_->password(var_cfg_requirepass ? var_cfg_requirepass : "");
    const auto acl_error = service_->acl_file(
        var_cfg_aclfile ? var_cfg_aclfile : "");
    if (!acl_error.empty()) {
        logger_error("ACL startup failed: %s", acl_error.c_str());
        exit(1);
    }
    service_->replica(var_cfg_replica_rpc_source &&
        *var_cfg_replica_rpc_source);
    service_->replica_read_only(var_cfg_replica_read_only != 0);
    move_domain_.reset(new move_domain);
    const auto domain_error = move_domain_->prepare(var_cfg_dbtype,
        var_cfg_dbpath, static_cast<size_t>(var_cfg_databases),
        var_cfg_move_journal_path ? var_cfg_move_journal_path : "");
    if (!domain_error.empty()) {
        logger_error("MOVE domain startup failed: %s", domain_error.c_str());
        exit(1);
    }
    prepare_watcher();
    db_ = create_database(0);
    if (!db_) exit(1);
    databases_.reset(new database_pool(db_,
        static_cast<size_t>(var_cfg_databases),
        [this](size_t index) { return create_database(index); },
        existing_databases()));

    const auto bind_error = move_domain_->bind();
    if (!bind_error.empty()) {
        logger_error("MOVE domain binding failed: %s", bind_error.c_str());
        exit(1);
    }
    if (auto* journal = move_domain_->journal())
        move_recovery::run(*databases_, *journal);
    service_->set_move_journal(move_domain_->journal());

    acl::string buf(var_cfg_service_addr);
    auto& tokens = buf.split2(":|");
    if (tokens.size() != 2) {
        logger_error("Invalid service=%s", var_cfg_service_addr);
        exit(1);
    }

    char* end;
    int service_port = (int) std::strtol(tokens[1].c_str(), &end, 10);
    if (*end != 0) {
        logger_error("Invalid port=%s, service addr=%s", end,
            var_cfg_service_addr);
    }
    if (service_port <= 0 || service_port >= 60000) {
        logger_error("Invalid port=%d, service addr=%s",
            service_port, var_cfg_service_addr);
    }

    int rpc_port = service_port + SERVICE_RPC_PORT_ADD;

    manager_.init(var_cfg_redis_max_slots, var_cfg_cluster_mode,
                  var_cfg_cluster_path, tokens[0].c_str(),
                  service_port, rpc_port, db_, databases_,
                  var_cfg_masterauth ? var_cfg_masterauth : "");
    if (!pkv::enable_shutdown_signals([this](int received) {
        shutdown(received);
    })) {
        logger_error("Cannot enable shutdown signal callback"); exit(1);
    }
}

void master_service::close_db() {
    expiry_.reset();
    databases_.reset();
    if (db_) {
        db_ = nullptr;
    }
}

void master_service::proc_on_exit() {
    logger(">>>proc_on_exit<<<");
    try { shutdown(0); }
    catch (...) {
        logger_error("Service exit callback failed");
        _exit(1);
    }
}

bool master_service::proc_on_sighup(acl::string&) {
    logger(">>>proc_on_sighup<<<");
    return true;
}

std::once_flag watch_once;

// One watcher for watching the changing of db.

void master_service::thread_on_init() {
    service_->statistics().worker(pkv::client_worker_id());
    // Prepare the cache object first for each thread.
    ocache = new redis_ocache(var_cfg_ocache_max);
    assert(ocache);

    for (int i = 0; i < var_cfg_ocache_max; i++) {
        auto o = new pkv::redis_object(*ocache);
        ocache->put(o);
    }

    std::call_once(watch_once, [this] { start_watcher(); });

    assert(watcher_);

}

void master_service::prepare_watcher() {
    watcher_ = new slave_watcher(var_cfg_replica_publish_buffer_limit,
        var_cfg_replica_pending_messages);
    assert(watcher_);
    watchers_->add_watcher(watcher_);
    service_->set_replication_watcher(watcher_);
}

void master_service::start_watcher() {
    std::lock_guard<std::mutex> lifecycle(lifecycle_lock_);
    if (stopping_) return;
#undef __FUNCTION__
#define __FUNCTION__ "start_watcher"
    assert(watcher_);

    // Start one thread for acceptint connection from other nodes.
    std::thread thr1([this] {
        // Start one server fiber for every thread to handle the process
        // betwwen different nodes.
        go[this] {
            pkv::cluster_service service(*watcher_, &service_->passwords());
            acl::string addr;
            addr.format("%s:%d", manager_.get_service_ip(),
                manager_.get_rpc_port());
            // Bind the local address to accept connection from other nodes.
            if (!service.bind(addr)) {
                logger_error("Bind %s error %s", addr.c_str(),
                    acl::last_serror());
            } else {
                logger("Bind rpc addr=%s ok", addr.c_str());
                service.run();
            }
        };
    
        acl::fiber::schedule();
    });
    thr1.detach();

    // Start one thread to wait db messages and forward them to the clients.
    std::thread thr2([this] {
        // Start one watcher fiber to wait messages from box.
        go[this] {
            watcher_->run();
        };

        acl::fiber::schedule();
    });
    thr2.detach();

    if (var_cfg_active_expire &&
        (!var_cfg_replica_rpc_source || !*var_cfg_replica_rpc_source)) {
        expiry_.reset(new expiry_service(databases_,
            static_cast<size_t>(var_cfg_expire_batch),
            static_cast<size_t>(var_cfg_expire_interval)));
    }

    // This is an opt-in live update stream, not Redis full synchronization.
    if (var_cfg_replica_rpc_source && *var_cfg_replica_rpc_source) {
        const std::string source(var_cfg_replica_rpc_source);
        auto primary = db_;
        auto databases = databases_;
        const std::string password(
            var_cfg_masterauth ? var_cfg_masterauth : "");
        std::thread receiver([source, primary, databases, password] {
            go[source, primary, databases, password] {
                auto client = std::make_shared<cluster_client>(source, primary,
                    databases, password);
                client->open();
                client->run();
            };
            acl::fiber::schedule();
        });
        receiver.detach();
    }

}
