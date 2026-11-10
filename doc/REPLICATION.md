# Versioned live replication stream

pkv has a custom live RPC update stream, not Redis PSYNC/REPLCONF replication.
Version 1 adds negotiated framing and logical database identity. Version 2
adds atomic transactions of bounded physical pages. It can propagate
new changes across DB 0 and nonzero DBs. It does not provide an initial snapshot or reconnect replay.

## Negotiation and framing

The receiver requests `PKV-REPL 5\r\n`. The publisher enqueues READY and
registers the subscriber before sending READY or subsequent records. An
unnegotiated connection is never added to the publication queue. The
greeting is bounded to 64 bytes and uses a five-second read timeout.
Duplicate requests for the same version retain the existing READY barrier.
Changing versions on an established connection closes it.

Roots and pages are published in their native binary representation,
including compact records. Page and EXEC groups are applied only when
complete. Unsupported roots, including old JSON records, close the stream
before the corresponding group is applied. A peer without page support
is disconnected when a page transaction cannot be represented. READY
orders the live stream without providing a snapshot or replay offset.

See [page replication](PAGE_REPLICATION.md) for frame bounds and
[PKV5](COMPACT_ERASE_REPLICATION.md) for grouped erasures.

The version 1 header is also 20 bytes, with five network-order 32-bit fields:

| Position | Field |
| --- | --- |
| 0 | Magic `0x504b5631`, the bytes `PKV1` |
| 1 | Operation: SET=1, GET=2, DEL=3, READY=4 |
| 2 | Logical database index, 0 through 65535 |
| 3 | Key byte length |
| 4 | Value byte length |

READY must have DB 0 and empty payloads. Unsupported magic/operations, invalid
READY and out-of-range DB fields are rejected. A receiver accepts DB N only after
READY, rejects DB indexes outside its configured pool and resets negotiation
state on every reconnect. Cluster receivers reject DB N. An old reader receiving
a versioned frame would reject its magic as an invalid legacy operation; the DB
identifier is not hidden in reserved legacy fields that old readers ignored.
Native tests check exact network-order bytes independently of the decoder.

Empty and arbitrary binary keys/values work in both formats. The reader clears
old output on every attempt and rejects malformed/truncated frames. Existing
limits remain 65,525 key bytes and 1,024,000 value bytes; DEL has no value. Senders
validate whole batches before writing/narrowing lengths and enforce the stream's
int-sized write bound. Allocation uses the actual encoded size; the old explicit
capacity overload cannot underallocate its buffer. Full byte counts are checked.
Network failure can leave a partial frame, so a failed stream is closed.

## Configuration, observers and apply

For an opt-in live receiver on an independent server, configure its upstream RPC
address (the upstream Redis service port plus 10000):

```text
replica_rpc_source = 127.0.0.1:29001
```

The default is empty. Existing cluster-managed receivers also request version 1.
Do not combine this custom source setting with a separately managed subscription
to the same upstream. This setting starts a live receiver, not a Redis-compatible
replica role, read-only mode, or full synchronization. Matching logical DB counts
and compatible stored codecs are required; opaque record payloads are not codec
negotiation or transcoding. Data committed before a ready subscription may be
absent. Prepare initial data separately and do not infer recovery guarantees.

Each DB's observer dispatcher carries its DB index. New database-aware hooks
preserve old custom observers by calling their legacy hooks only for DB 0.
Lazily created DBs retain shared ownership of their dispatcher, avoiding borrowed
metadata lifetime problems. The common publication FIFO carries the database
index with every successful SET/DEL. Reads are not published. A legacy recipient
rejects/drops nonzero DB events; upgraded recipients retain them.

The receiver selects the corresponding shared DB from its pool and uses the
same key locks as commands. DB N never falls back to DB 0. Replica application
bypasses observers so it does not echo updates. Deletion is idempotent when the
key is already absent, including after lazy expiry; backend lookup/delete errors
still fail. Application or framing failure causes reconnection, with a new
handshake. There is no automatic replay of the failed record.

## Queue ownership and order

The subscriber list is protected while taking shared-pointer snapshots. Each
recipient owns one message reference; a closed recipient releases rejected
pushes. Closing serializes with enqueue and wakes the writer; pending messages
are drained. Flush releases/clears its batch on success and failure, preventing
double release. Each subscriber uses a fiber-aware queue, allowing another
subscriber to register while it is idle.

One publication FIFO preserves same-key commit order across worker threads
because successful mutators enqueue while holding key locks. This does not make
multi-key batches atomically visible to a receiver: events are individual
records without a transaction envelope.

## Pending-data limits

The following positive integer settings bound encoded pending data:

```text
replica_output_buffer_limit = 67108864
replica_publish_buffer_limit = 67108864
replica_pending_messages = 65536
```

The byte defaults are 64 MiB. Each record is charged its 20-byte header plus
key/value bytes, including READY. Both queues count queued and in-flight records
until forwarding or sending finishes. The message limit applies independently
to the publisher and each subscriber. Configuration accepts values from 1 through
2147483647. Small limits can disconnect a healthy subscriber during a burst.

An individual output overflow closes that subscriber and drains its references.
A publication overflow disconnects all current subscribers: the source write is
already committed, so retaining those streams would silently omit an update.
An epoch barrier discards old queued publications before a new subscriber can
receive them. Blocking sends are interrupted by socket shutdown, with final close
serialized against enqueue and shutdown. Source commands continue after either
kind of overflow. Reconnection provides no missing-data replay or automatic
resynchronization; a disconnected replica needs a separately prepared snapshot.

These limits charge encoded bytes, not resident memory. Allocator overhead,
shared-reference overhead, connection count and total server memory are not
bounded by these settings. Queue contention and throughput still need measurement;
this does not establish general allocation-failure or graceful-shutdown safety.

## Verification and remaining work

Both C++17 ASan/UBSan and C++11 Release run all twenty-four CTest entries on the tested
Unix host. Native transport tests cover single/batched empty/binary payloads,
maximum lengths, fragmented/truncated input, invalid legacy and versioned
headers, exact versioned bytes, failed-flush cleanup, and pool-scoped application.
They reject DB N before READY, duplicate READY and out-of-range DBs. Storage
fixtures check observer lifetime/identity, no apply echo, idempotent deletion and
failed DB N writes/deletes.

The real TCP suite contains 420 legacy stream checks plus 18 upgrade checks,
12 two-process multi-DB apply checks and eight reconnect/legacy-upstream checks.
It tests DB 0/1/7/15 same-name isolation and exact TTLs, typed updates/deletions,
cross-DB COPY, rename events, legacy filtering, unknown upgrade versions and
negotiation reset on reconnect. An old upstream can still deliver DB 0 frames
after ignoring the greeting. Four writers produce 400 ordered increments during
25 connection resets in the retained legacy suite. These are custom pkv RPC
checks, not official Redis replication parity or production recovery evidence.

The queue suite adds 1,521 independent checks, including strict per-subscriber
and publisher overflow, post-overflow registration, a READY below the byte budget,
and 1,500 ordered writes to a healthy peer while a stalled peer is disconnected.
Native fixtures verify exact byte/count boundaries, arithmetic overflow,
in-flight accounting, wakeup of blocked writes and stale-publication rejection.

The complete-server objective still requires initial synchronization, offsets,
acknowledgements, reconnect replay, authenticated peers, codec compatibility
policy, durable failure recovery, transaction boundaries, global memory policy, replica
roles/health and orderly shutdown. Large valid RESP records can exceed the current
RPC value limit; chunking or negotiated size handling remains needed. Distributed
clock/durability behavior and disk-engine replica reopening are unverified.

## Atomic page transactions

PKV2 is connected to negotiation, publication/output queues and the live receiver.
Native page changes are sent as one BEGIN/root/page/COMMIT group and applied
atomically. Queue budgets charge all group bytes and frames, including controls.
Legacy/v1 peers disconnect when a page event they would need cannot be represented;
legacy DB N filtering remains unchanged. New receivers prefer v3, then retry v2 and v1
when an old upstream closes before READY. Pending transactions are discarded on
reconnect. See [Page replication](PAGE_REPLICATION.md) for protocol details and
native/TCP evidence. List commands now use bounded pages on native `mdb`.
Initial synchronization, replay and disk recovery remain open.

Client MULTI/EXEC has in-process execution isolation. PKV3 now publishes one
whole EXEC group and applies single-database groups in one destination commit.
Mixed-database groups reject before any write. All-path source whole-EXEC durability,
cross-database recovery and reconnect replay remain open. ROOT_MAINTENANCE
preserves WATCH during legacy format migration; older v2 receivers reject it.
See [EXEC replication](EXEC_REPLICATION.md) and [transactions](TRANSACTIONS.md).

Eligible native source EXEC groups now stage and commit once before success
replies. See [source commit routing and remaining fallbacks](SOURCE_EXEC.md).
Cross-database/physical-store and legacy whole-value key-operation paths still need
coordinated recovery or migration.
