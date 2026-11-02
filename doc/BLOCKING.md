# Blocking lists

PKV supports BLPOP, BRPOP, BLMPOP, BLMOVE, BRPOPLPUSH and CLIENT UNBLOCK
with TIMEOUT/ERROR. These commands use the existing native paged List format
on MDB, oneTBB, RocksDB and WiredTiger. Roots and pages remain bounded at
16 KiB; larger elements use chunks rather than a serialized collection blob.
Blocking sorted sets are covered in [their own scope](BLOCKING_ZSET.md).
Streams remain pending.

## Protocol and selection

Decimal timeouts round upward to milliseconds and use one steady-clock
deadline. Zero means indefinite. Spurious wakes and bounded internal waits
do not restart the deadline. EXEC runs these commands without waiting.
RESP2 timeout replies are null arrays, including BLMOVE and BRPOPLPUSH;
nonwaiting misses for those two commands inside EXEC are null bulk strings.
RESP3 represents both as null. Parsing, error precedence and reply types are
checked against the local modified Redis 8.6.2 reference.

Waiting clients subscribe before their initial locked data probe. Each DB has
its own queues; duplicate binary keys occupy one node. Initial probes respect
older waiters. A real list producer selects eligible clients in publication
order. After waking, the selected client re-executes its original key order.
For example, a waiter on `a b` followed by EXEC with SET a to a string,
RPUSH b, DEL a and RPUSH a wakes because of b, but ultimately pops a.
The wake trigger and final pop key are separate decisions.

An eligible wake reserves all keys in that request until its locked probe
finishes. This preserves reference behavior for overlapping requests while
letting disjoint requests proceed concurrently. An unsuccessful actual
re-execution rejoins at the tail. Cancellation probes are distinct from real
producer events and cannot acquire producer priority.

## Admission, commits and storage

Ordinary commands retain shared execution admission. EXEC retains exclusive
admission and its frozen command clock. `execution_pause` releases all nested
shared admissions for the waiting thread/fiber actor and restores their exact
depth before storage access. It rejects missing or exclusive admission.
Notifications inside EXEC cannot expose intermediate transaction state.

Typed readiness tracks final List type and expiry. SET, DEL, expired values
and discarded workspace changes cannot create false producer events. Ordered
workspace and private RPC commits preserve the first positive list producer's
position while checking the final value. Readiness preparation happens before
the native commit; publication happens only after success. Failed native
writes preserve storage, WATCH state and waiter notifications.

The lock order is execution admission, DB key locks, then waiting registry.
Registry code never accesses storage or invokes consumers. It uses ACL fiber
mutexes/conditions and releases its mutex before resuming execution admission.
Publication and cancellation require no C++ allocation, including a request
with 4096 keys. An iterative dispatch queue avoids recursive wake chains.
Unsubscribed databases take a fast no-subscriber path. Actor-local workspaces
cannot subscribe to source DB queues.

## Connections and cancellation

A connection that actually begins waiting gains one ACL reader fiber with a
64 KiB stack. This fiber becomes the sole socket reader and buffers raw input
in chunks of at most 16 KiB, with a 1 GiB byte/heap quota. Parsing and command
execution stay in the original connection actor. Commands pipelined after a
blocked request remain deferred; replies preceding it are flushed first.
Normal connections do not create this extra fiber.

Peer EOF, CLIENT KILL, CLIENT UNBLOCK, allocation failure and input quota
failure release the wait safely. Reader startup, shutdown before scheduling
and shutdown after completion have explicit lifecycle states. A retained
control object holds a weak queue token, so it cannot retain FIFO membership
after the command ends. Unblock only succeeds while the command is waiting.
ACL and replica write policy are checked again before an actual resumed pop.

INFO reports blocked clients and CLIENT INFO/LIST includes the `b` flag.
Timeout and explicit unblock count as completed commands; UNBLOCK ERROR also
counts as failed. Disconnect and kill do not count a pending command as
executed. Permission rejection after waking increments rejected calls.
Command execution time excludes time suspended waiting for data.

## Validation

The protocol suite checks 429 stable and 20 typed differential observations,
plus FIFO/overlapping waiters, pipeline ordering, 40,000-byte binary elements,
ACL revocation, replica policy changes, WATCH, DB isolation, cluster errors,
disk restart and grouped private RPC replay. Six additional command-statistics
cases cover timeout, both unblock modes, kill, EOF and ACL revocation.

Native tests cover four-thread/sixteen-fiber handoff, execution admission,
expiry, actual versus cancellation events, 4096-key allocation-free dispatch,
128 storage fault budgets, 256 queue construction budgets and 32 reader startup
budgets. Optional backends exercise their actual storage implementations.
The separate killed-waiter probe checks that cancellation resumes only after
another actor leaves exclusive admission.

Matched hot inline GET medians relative to the preceding PKV revision:

| Backend | RESP2 | RESP3 |
| --- | ---: | ---: |
| MDB | +0.273% | -0.642% |
| oneTBB | +0.624% | -0.301% |

Each backend checked 50,331,648 replies across three rotating samples per
variant. These small differences do not establish statistical significance.
The preceding List blocking revision passed an eight-configuration matrix
of 986/986 tests: MDB Debug/C++11
112 each, oneTBB Debug/C++11 123 each, RocksDB Debug/Release 129 each, and
WiredTiger Debug/C++11 129 each. Debug configurations include ASan/UBSan.
All 168 recorded source fingerprints remained fixed during final validation;
benchmark binaries and compile flags match the full-matrix Release builds.
Both freshly linked cancellation-isolation probes also passed.

That revision's matrix and matched inline GET measurements are recorded in
[the validation report](benchmarks/blocking-validation-2026-11-01.json).
The comparison uses PKV revisions on macOS ARM64 and a local modified Redis
reference for correctness. It does not establish a speed advantage over stock
Redis, mixed workloads, paged reads or disk workloads. Full Redis compatibility
remains an active goal.

Current waiting-index ownership and scaling checks are recorded in
[queue validation](QUEUE_SCALING.md), including the 1024-test matrix.
