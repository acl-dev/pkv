# Process shutdown and exit regression

The POSIX standalone service now consumes SIGTERM and SIGINT on a normal
thread, drains command admission, stops active expiry and closes opened native
stores before exiting. This fixes a reproduced normal-stop SIGABRT. It does
not change command framing, serialization, page layout or memory containers.

## Reproduction and cause

The unchanged ownership652 source was built as a separate C++11
RelWithDebInfo memory server. Six isolated SIGTERM cases, with active expiry
off/on, all returned `-6` without a forced kill. The new lifecycle fixture also
failed against that unchanged binary rather than accepting its command replies
as sufficient evidence of success.

Seven matching baseline dumps were symbolicated with their actual executable.
Each showed this application-side call chain:

```
msg_fatal
kqueue_wait
event_process
fiber_io_loop
fiber_start
```

The installed matching ACL source explains the race. Its default SIGTERM
handler closes descriptors 0 through 1023, sleeps briefly and then calls
`_exit(0)`. PKV still has worker and background coroutine schedulers running.
A scheduler can call kevent on its closed kqueue descriptor and enter
`msg_fatal` before the signal handler reaches its successful exit.

The ten older retained reports identify the WiredTiger C++11 PKV executable,
not the Codex executable, as their main module. They cannot classify the
already deleted historical reports. The new isolated memory reproduction
establishes that this normal-stop problem is not limited to WiredTiger.

## Signal and native resource ordering

Main blocks the two process shutdown signals before initializing ACL or
creating worker threads. Worker threads inherit that mask. A dedicated normal
thread consumes a signal using `sigwait`; the ACL asynchronous handler does
not close scheduler descriptors. SIGABRT and other fault signals remain
unchanged, including in intentional crash tests.

In the initial shutdown660 validation, the waiter started after service
initialization and earlier signals remained pending. The later
[startup cancellation change](STARTUP_CANCELLATION.md) starts it before ACL and
configuration loading, then publishes the normal shutdown callback only after
service initialization. Its startup interruption and recovery checks are
separate from the original normal-signal matrix below.

Shutdown sets the lifecycle stopping flag under its startup mutex, takes
ownership of the expiry service, then joins that service outside the mutex.
Expiry is stopped before acquiring exclusive execution admission because it
may itself be waiting for ordinary command admission. Concurrent watcher
startup checks the same stopping flag and cannot restart expiry afterward.

The shutdown thread then acquires exclusive execution admission. This drains
commands and prevents new DB operations. While retaining that admission it
locks the DB pool, disables its lazy factory and closes every opened DB:

- Memory engines retain their existing containers until process reclamation.
  No collection serialization or snapshot is introduced by this exit path.
- RocksDB flushes each physical store's WAL with synchronization, then releases
  its column-family handles and DB objects. A reported flush error is retained.
- WiredTiger releases cached sessions/cursors and checks the connection close
  result. Its destructor can subsequently run without closing those handles
  twice.

Untouched, unopened disk databases do not need to be opened during shutdown.
A close error does not prevent attempts to close the remaining opened DBs.
Successful closes update the existing opened flag; no ordinary key/frame or
native memory-container fields are added.

After logging the result, the process exits with status zero on success and
one on a close/callback failure. Exclusive admission remains held through
`_exit`; it is never released after native handles have been closed. ACL's
existing detached scheduler/RPC threads are reclaimed by process exit, rather
than destroyed underneath running command code. This is a terminal process
operation, not a reusable close/reopen API.

## Tests and evidence scope

`stop_server` in the integration fixture now requires status zero and rejects
a timeout followed by a forced kill. If a test body has already failed, its
original exception remains primary and shutdown diagnostics are printed.
Consequently, passing protocol assertions cannot hide an abnormal service
exit as they did in the previous fixture.

`server_exit` covers SIGTERM/SIGINT, expiry off/on, a waiting BLPOP client,
listener release, and large String/Hash/List/Set/ZSet data in logical DBs 0 and
7. Disk cases reopen the same storage and compare the acknowledged values.
`database_shutdown` injects native close failures and exceptions, verifies
that all DBs are attempted, checks successful idempotence and disabled lazy
opening, and checks that an active shared command finishes before exclusive
shutdown proceeds.

The initial memory and WiredTiger C++11 focus each passed 15 selected checks.
The expanded matrix built all targets in eight configurations and passed 506
selected CTests: MDB 55/55, oneTBB 63/63, RocksDB 67/67 and WiredTiger 68/68.
Four Debug configurations use ASan/UBSan. This is not the complete CTest suite.
The six matching after-fix SIGTERM cases all exited zero, with no new matching
dump. The compiler options/defines match the before build; a CMake-added explicit
include argument differs, while 271 shared object dependency files resolve the
same local headers.

The initially restored RocksDB Release library disabled RTTI by default. Its
typeinfo was required by UBSan vptr checks, so that sanitizer build initially
failed to link. A separate same-version library with RTTI enabled resolved it.
The original failed build remains recorded; the five unaffected configurations
were retained and the three remaining configurations completed in a separate
run. No PKV source or sanitizer checks changed for that dependency repair.

Matching service binaries and UUID-checked dSYM bundles, source archive, parent
file copies, dependency libraries and logs are retained under
`build/validation/shutdown-2026-11-02` (about 477 MiB, ignored build output).
Raw memory dumps remain in Crashpad. Some original LTO debug information is
unavailable; generation warnings are retained, and the saved baseline dSYM was
checked to resolve the key fatal/kqueue stack. Snapshots are local diagnostic
artifacts, not a portable installer.

The [validation report](benchmarks/shutdown-validation-2026-11-02.json) records
exact selections, retained paths, source/build hashes and the before/after
evidence. First focus checks and the initial dependency failure are separate
from the final successful matrix.

Windows retains its previous signal branch and is not validated by these
POSIX checks. The later [callback shutdown change](CALLBACK_SHUTDOWN.md) routes
ACL idle/use/supervisor exits through the same terminal operation and adds
separate native-cleanliness checks. The signal matrix above predates that
change. This change does not establish full Redis
SHUTDOWN command semantics, outgoing replication-queue drain, memory
snapshots, PSYNC, or complete durability under power loss. Historical RocksDB Release startup failures cannot be
claimed fixed from newly rebuilt dependency libraries alone.

See [stages and remaining estimates](ROADMAP_STATUS.md) for the full active
goal, and [ownership replay](OWNERSHIP_REPLAY_VIEW.md) for the subsequent MOVE
integration work.
