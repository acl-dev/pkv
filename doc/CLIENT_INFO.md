# CLIENT INFO and LIST

CLIENT INFO returns one newline-terminated row for the current public Redis
connection. CLIENT LIST returns rows for public connections, with optional
`TYPE <type>` or `ID <id> [id ...]` filtering. ID requests preserve input order
and duplicates. IDs use canonical signed-64-bit decimal; zero/negative IDs are
valid numbers with no matching current connection. Filters cannot be combined.
All ID arguments are validated before constructing rows.

Both commands return RESP2 bulk text or RESP3 verbatim `txt` text. HELLO/RESET
pipelines and EXEC children retain their selected protocol. COMMAND metadata
and CLIENT HELP advertise only implemented children. Valid normal filters match
unsubscribed frontend connections. The pubsub filter matches active ordinary,
pattern or shard subscriptions; master and slave/replica match none.
The companion RPC transport is not a public Redis connection. The user field
now reports the actual [ACL identity](ACL_PERMISSIONS.md). Local
[Pub/Sub](PUBSUB.md) is implemented; full Redis replication remains unfinished.

## State and synchronization

Rows include the existing immutable ID, numeric endpoints, descriptor, creation
time and worker identity. Name, protocol and library attributes use the copied
metadata view from [the snapshot infrastructure](CLIENT_SNAPSHOTS.md). A record
holds its lifetime mutex while invoking the runtime observer, and retirement
detaches the observer before handler/parser destruction. Retained records cannot
access destroyed state. Missing/retired observations are skipped.

Parser update and request release share a connection-local inspector mutex with
resource traversal. Parsing and traversal contain no backend or network IO.
Queued transaction mutations and WATCH membership changes use this same mutex;
backend watch/subscription work happens before taking it. Queue movement into
EXEC publishes a scoped pointer and clears it before the local queue dies.
No standard mutex spans command execution, storage IO or response writes.

The parser tree remains read-only during dispatch. Memory traversal runs only
when queried; fragmented input does not trigger repeated tree scans. It visits
both completed requests and the unfinished root, including the current partial
child. Response-building trees are not traversed concurrently. Redis-object
layout is unchanged: resource methods add no per-object fields.

Lightweight atomic gauges publish DB selection, current argument bytes, stable
command-name pointers, completed command counts and IO totals. One owner writes
each activity record; cached command/argument values avoid redundant stores.
Ordinary commands gain no metadata mutex or memory-tree traversal. Transport
accounting and synchronization occur per read/write batch. Individual gauges
and different client rows need not describe one global instant.

## Field meanings

Names, DB, protocol, library attributes, current command, MULTI count and WATCH
flags follow the exercised Redis behavior. During EXEC, cmd remains exec,
multi remains the full queued count, and argv-mem covers the queue payload.
Queued commands increment tot-cmds when executed, not when appended. INFO sees
the completed prefix and does not include itself. Rejected unknown/arity/NOAUTH
requests do not increment completed commands. Body errors do increment it.

Idle MULTI has argv-mem=0 and multi-mem equal to queued argument payload bytes.
A dirty WATCH reports flag d and zero logically active watches; PKV may retain
the invalid tokens until UNWATCH/EXEC/DISCARD/RESET. The observer never changes
WATCH outcomes. The x/c/A/U flags reflect transaction, deferred/immediate close
and Unix-socket state. P reports active Pub/Sub membership; sub/psub/ssub count
ordinary, pattern and shard subscriptions. Tracking has no redirect target.
Push delivery does not increment completed commands or clear arguments retained
by a blocked command.

Allocation representations differ from Redis. These fields report PKV data,
not Redis allocator sizes:

| Field | PKV measurement |
| --- | --- |
| qbuf | String bytes in the unfinished decoded request tree. |
| qbuf-free | Unused string capacity in that tree, including inline capacity. |
| argv-mem | Current dispatched payload bytes; queued payload while in EXEC. |
| multi-mem | Sum of queued command/argument byte lengths. |
| rbs | Capacity of the active contiguous serialized output buffer. |
| rbp | Peak serialized output length observed since connection creation. |
| obl | Active serialized bytes plus retained outbox frame bytes. |
| oll | Retained outbox frames, including an active writer frame. |
| omem | Serialized buffer heap plus estimated retained outbox heap. |
| tot-mem | Known-owned memory estimate described below. |
| events | Read service remains enabled; w denotes an active write call. |
| tot-net-in/out | Successful bytes reported by completed ACL IO calls. |

Completed pipeline frames are held as decoded arguments, not a raw Redis SDS
query buffer. Their allocations enter tot-mem, while qbuf describes only the
unfinished root. Readiness and incomplete-frame observations therefore differ
from Redis's untrimmed raw query buffer. A blocked output call retains the full
serialized buffer even when some bytes have entered the kernel; obl is not a
kernel-unsent-byte count. Failed partial writes can retire before their byte
progress becomes observable.

The owned estimate includes handler/record storage, transport buffer capacity,
parser nodes/vectors/heap strings, queued vectors/strings, WATCH map payloads,
metadata/endpoint heap strings and published serialized output storage. It
excludes allocator rounding, map/control-block bookkeeping, shared per-thread
object caches, transient reply
or workspace trees, opaque subscription-registry allocations, stack locals and
kernel socket buffers. It is not process RSS or an exact allocator total.
String allocation estimates distinguish inline storage from heap capacity.

## Verification and limits

The network fixture compares 121 stable field/error assertions with the local
modified Redis 8.6.2 reference, then checks duplicate/order-preserving filters,
actual endpoints, WATCH invalidation, idle MULTI and EXEC prefix counters,
network-byte increments, partial frames, concurrent LIST/HELLO/disconnect,
requirepass, RESP3 and real redis-cli. An 8 MiB backpressure case requires
observable output storage while another connection can query the writer.
The value continues to use bounded String pages; these commands change no
persisted encoding or maximum List/Hash/Set/ZSet page size.

Native fixtures check activity counters and observer retirement. The parser
fixture races real fragmented parsing/resource traversal and releases its
context after retirement. Standalone TSan runs instrument these fixtures and
the copied metadata fixture; the ACL archive remains uninstrumented and these
are not whole-fiber-server TSan claims. Exact framing tests cover binary
verbatim payloads, RESP2 fallback and existing cache variants.

Results, source fingerprints, build logs and measurement limits are recorded in
[the validation report](benchmarks/client-info-validation-2026-10-31.json).
The overall Redis-server goal remains open, including full ACLs, blocking
commands, additional data/expiration families and complete Redis replication.


Large response delivery uses bounded writes that fit ACL's signed-int return
range. Offset/progress accounting stays size_t, including lengths above INT_MAX.
A boundary fixture checks these lengths without allocating multi-gigabyte
payloads and checks partial, closed and over-reporting senders. LIST text and
its serialized response are still materialized in memory; output is not a
streaming row iterator and large LIST requests can require substantial memory.


The final revision passed 766/766 CTests across eight memory, oneTBB, RocksDB
and WiredTiger configurations, including C++11 builds and ASan/UBSan Debug
builds. Three independent TSan fixtures and twenty repeated/relinked metadata
allocation-failure checks passed. The retained earlier allocation-hook anomaly
is documented in [the investigation](CLIENT_INFO_OOM.md); its exact runtime
cause is unresolved. These results establish the exercised scope, not complete
Redis compatibility.


## Checked GET revision comparison

Three rotating rounds checked 37,748,736 GET replies across the retained
snapshot revision (RESP2) and this revision (RESP2/RESP3). Both use the same
C++11 Release core flags, memory backend, two server threads, four clients,
256-request batches, 64-byte payload and disabled active expiration. Median
throughput was about 1.529 million requests/s for the baseline and 1.528
million for new RESP2 (about -0.02%); new RESP3 was 1.521 million
(about -0.48%).
This workload observed effectively unchanged throughput, without statistical
significance or general speed claims. It does not represent mixed workloads,
other backends or stock Redis. See [raw samples and fingerprints](
benchmarks/client-info-get-2026-10-31.json).
