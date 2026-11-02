# Pub/Sub output transport foundation

The initial increment added `common/connection_output.*`, a bounded output
transport, before any command integration. The [current Pub/Sub implementation](
PUBSUB.md) now connects it to the broker, protocol frames and channel ACLs.
The original validation below describes the transport-only stage; its server
binaries did not yet link the component. Never-promoted connections retain
the direct writer.

## Ownership and ordering

One connection owner starts a writer fiber on its own ACL scheduler. All
socket writes then belong to that writer. Producers on other threads append
immutable shared strings under the queue lock; they never perform socket
I/O or wait for a slow recipient. The consumer releases the lock before
calling the sender. The transport does not create an OS thread per connection.

The owner reserves a reply ticket before producing a command reply. This
inserts an unfilled placeholder, so subsequent pushes cannot overtake the
reply. Nested execution reuses the pending ticket, allowing EXEC to retain
one outer reply boundary. Fulfilling a ticket supplies the completed bytes;
waiting for it covers the reply and its captured prefix, without waiting for
an endless stream of later pushes.

Subscription changes will reserve and capture while holding the broker lock.
Capture moves earlier external pushes ahead of the reply placeholder, in
publication order. Self-origin pushes remain after the reply, preserving the
reference server's deferred self-message behavior inside EXEC. Repeated
capture preserves prefix order and does not allocate. This transport only
orders encoded frames; constructing RESP2 arrays and RESP3 pushes remains
the command layer's responsibility.

## Bounds and shutdown

The default quota is 32 MiB. Both payload bytes and estimated C++ allocation
bytes must fit independently. Each queue node, unfilled reply and empty push
is charged. Shared frames are conservatively charged to each recipient,
including the string's retained capacity and estimated control-block space.
The quota includes the entire in-flight frame until its send completes;
removing a frame from the linked queue cannot hide it from admission.
Allocator rounding, ACL allocations and fiber stack space are not measured
by this queue quota. `owned()` reports state and charged queue memory.

An enqueue allocation failure or quota violation closes the transport,
wakes waiters and releases queued ownership. Partial writes fail the frame
and close the connection. Successful earlier chunks remain in the reported
sent-byte count when a later send throws `std::bad_alloc`. Each individual
sender call is bounded to 16 MiB by `response_write`.

The close callback must interrupt an active socket write, normally using the
connection registry's shutdown operation. It runs outside the queue lock,
may run on a publishing thread and must not call `stop()` or throw. The sent
callback runs on the writer fiber and must not throw. The owner must retire
all external producers before destroying the transport and its callbacks.
Start, stop and ticket waits belong to the owner fiber.

Stopping first closes the transport and then waits for the writer to exit
naturally. It also works before the worker's first scheduling. It does not
kill a fiber waiting for a condition variable: a concurrent-close regression
exposed a fatal ACL mutex reacquisition when using synchronous fiber kill.
The finished condition notification avoids that path and joins before
callback targets can be destroyed. Repeated stop calls close only once.

Handler integration stops the writer while its registry entry still owns a
valid descriptor: `client_registry::retire` invalidates the shutdown target.
Output and endpoint objects remain alive until retirement excludes concurrent
inspectors. The handler retires broker producers first, stops the writer,
retires the client entry and only then destroys these observer targets.

## Validation

The `connection_output` native fixture checks:

- Unfilled, nested and repeated barriers, captured external pushes, deferred
  self pushes, completed versus invalid tickets, and post-close rejection.
- Four cross-thread producers with 4,000 queued frames and another 16,384
  frames consumed concurrently, preserving each producer's frame order.
- Binary data through real nonblocking socket pairs with a small send buffer,
  plus a stalled reader, quota shutdown and release of the active frame.
- Payload, retained-capacity, empty-frame, placeholder and reply quotas;
  allocation-free capture and cleanup; partial writes; chunked large replies;
  and allocation failure after an already delivered chunk.
- 32 writer startup allocation budgets, enqueue/reserve failure cleanup,
  stop before first scheduling, repeated stop, and 32 concurrent shutdowns.

The first RocksDB Release full run had one existing-server failure:
the existing socket fixture observed a server exit with signal 11 while
starting a `databases=3` instance. Its binary is byte-identical to the prior
validated server and does not link this transport. Five repeats of that
fixture and 64 independent starts of the configuration passed; the complete
RocksDB Release rerun also passed. The cause of the initial failure remains
unknown. Passing repeats do not resolve it; the validation report retains
both the failure and subsequent checks.

The final eight-configuration matrix passed 1,032 tests: MDB 117/117,
oneTBB 129/129, RocksDB 135/135 and WiredTiger 135/135. Each pair includes
an ASan/UBSan configuration and a Release configuration; RocksDB Release uses
C++17, while the other Release configurations use C++11. Five additional
ASan/UBSan repeats of the output fixture passed. All 541 frozen source hashes
matched at final validation. See the [validation report](
benchmarks/output-validation-2026-11-01.json) for configurations, binaries,
logs, fault budgets and the retained startup failure.

All eight server executables are byte-identical to the prior queue matrix:
the new unreferenced archive member is not linked into them. A server
throughput rerun would exercise the same binaries. This fixture validates
the future transport and makes no Redis performance comparison.
