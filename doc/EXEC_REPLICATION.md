# PKV3 whole-EXEC replication

A source EXEC collects successful mutations under its exclusive execution gate.
The publisher queues one immutable group only after execution finishes. Runtime
Redis errors still permit later queued commands to execute. Empty transactions and executions
without storage changes emit no replication event. A legacy read that migrates
its stored representation can still emit a maintenance batch. Ordinary commands retain page batches.
Subscriber admission shares the gate, so a new subscriber cannot join mid-EXEC.

PKV3 adds EXEC_BEGIN (14) and EXEC_COMMIT (15) around ordered native page batches.
The magic is 0x504b5633; the 20-byte header layout remains unchanged. Both outer
controls use DB zero, an empty key and zero page id. BEGIN has a 16-byte payload:
big-endian u32 nested batch count, u32 total frame count, u64 total wire bytes.
COMMIT has no payload. Counts include all nested headers and the two outer
controls (56 bytes). Groups are limited to 64 MiB and 65536 frames, including
these controls. Budgets account wire data, not allocator overhead or total RSS.
Inner frames use PKV3 and the existing page operations and validation rules.

Subscribers request `PKV-REPL 3` and await a versioned READY. A receiver tries
v3, then v2, then v1 only when the connection fails before READY. A negotiated
protocol failure never silently downgrades. Peers requiring v0/v1/v2 disconnect
before any EXEC bytes; unnegotiated DB-zero-only peers may ignore an entire group
that contains no DB-zero changes. Read-only EXEC does not disconnect old peers.
The sender validates all outgoing messages before writing even an earlier item.

The connection-local receiver owns nested batches until the outer COMMIT. It
checks declared budgets before reserving nested buffers. Malformed controls,
interleaved ordinary messages, truncation, allocation failure or inconsistent
counts discard the entire group and poison the receiver until reset. Incomplete
groups never reach storage. Reconnect resets pending input; it cannot recover
updates missed while disconnected.

A complete group targeting one logical database uses ordered page replay and one
native engine commit. Intermediate prefixes are validated under the union of key
locks. WATCH preserves logical write history, including creation and deletion of
an initially absent key. Single-database groups can target any configured DB.
Groups containing mutations in multiple logical databases are rejected before
any destination write. A SELECT with no mutation does not create a nested batch.
Independent physical stores also retain their existing all-or-nothing rejection.

Publisher/output limits reserve complete encoded charges before ownership is
transferred. Allocation failure, oversized legacy roots, shutdown or exception
unwinding invalidates affected subscribers and releases the whole capture.
An exception in a nested capture also poisons the outer group. Source writes
already committed remain committed. Subscriber mailboxes use RAII locking and
allocation-free close notifications, including under persistent allocation
failure; failed enqueue retains caller ownership and closes the subscription.
Legacy raw observer roots exceeding 16 KiB cannot be captured here. Native large
Strings remain supported through bounded pages.

## Remaining limits

Eligible source EXEC groups now use one engine commit; see
[source integration](SOURCE_EXEC.md). Cross-DB/physical-store, non-native and
opaque legacy COPY/RENAME fallbacks still can recover a source EXEC prefix.
There is no whole-EXEC source journal, cross-database commit/recovery coordinator,
initial synchronization, offset acknowledgement, replay, resynchronization or
Redis replication protocol implementation. A disconnected subscriber requires an
external consistent base before reconnecting safely. This change does not make
pkv a complete Redis service or establish a performance advantage over Redis.

## Tests

`exec_replication` covers framing, exact charges, old-peer rejection, mixed-DB
rejection before writes, malformed/truncated/interleaved groups, allocation
sweeps, publisher limits, EOF, observer/finalization allocation failure, nested
unwinding and mailbox allocation failure. `exec_live_stream` exercises real TCP
nodes, all native data types, large/binary values, runtime errors, WATCH history,
concurrent coherent reads, DB routing, reconnect discard and output overflow.
Existing page, transaction and backend replay tests remain required.

The final eight-build matrix passes 400/400 CTests. It includes C++11 builds,
oneTBB, RocksDB and WiredTiger, and ASan/UBSan server configurations. External
engine libraries are not sanitized. See [test evidence](../tests/README.md).
The memory EXEC unit was rebuilt after the final test-only line wrap; the
other seven complete runs use the final source. Logs are
`/tmp/pkv-exec-<build>-full.log` and
`/tmp/pkv-exec-memory-final-unit.log`.
