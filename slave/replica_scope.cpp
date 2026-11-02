#include "stdafx.h"
#include "replica_scope.h"
#include <exception>
#include "slave_watcher.h"

namespace pkv {
static int exception_depth() {
#if __cplusplus >= 201703L
    return std::uncaught_exceptions();
#else
    return std::uncaught_exception() ? 1 : 0;
#endif
}
replica_scope::replica_scope(slave_watcher* watcher)
: admission_(true), watcher_(watcher),
  owner_(watcher && watcher->begin_exec()),
  exceptions_(exception_depth()) {}
replica_scope::~replica_scope() {
    if (watcher_ && exception_depth() > exceptions_) watcher_->abort_exec();
    if (owner_) watcher_->end_exec();
}
} // namespace pkv
