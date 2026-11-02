# Replica client reads retain authoritative data

An expired key can be logically absent to a replica client while its physical
root and pages are still required by delayed primary updates. Replica reads
now retain that data. The policy matches Redis's distinction between logical
read expiry and primary-driven deletion; see the
[Redis replication expiration rules](https://redis.io/docs/latest/operate/oss_and_stack/management/replication/#how-redis-replication-deals-with-expires-on-keys).

## Reproduced loss

A standalone probe against the previous validated server core reproduced this
sequence using real handler commands and captured native page transactions:

1. The primary SETs a 70000-byte String with a deadline, updates one page with
   SETRANGE, then PERSISTs it before that deadline. Only the initial five-page
   value has reached the replica.
2. After the old deadline, a replica GET correctly returns nil but used to
   erase the root and all five pages, also publishing a deletion event.
3. The earlier SETRANGE and PERSIST reach the replica in order. They contain
   one changed page and metadata; the other four pages are not repeated.
4. The final String cannot be reconstructed from the damaged replica state.

The exact same probe source and compilation flags now retain all five page
allocations, emit no read-induced deletion and reconstruct the complete value.
The before/after exit codes are 1/0, respectively. Binary/core/runtime/source
fingerprints and logs are retained in the linked validation report.

## Command scope

`redis_handler::execute_one` enters `replica_read_guard` when the service is a
replica and the registered command is read-only. This includes individual
queued commands in a read-only EXEC and reads on a writable replica. The
existing READONLY admission check continues to reject unauthorized writes;
the guard is not a security boundary or an alternative write-permission check.

The guard uses a depth in the existing execution actor state. ACL fiber-local
storage keeps it isolated from sibling fibers on the same thread, and thread
fallback keeps ordinary threads separate. Nested/disabled scopes and exception
unwinding preserve the enclosing state. It does not acquire exclusive execution
admission and adds no key/record/shard fields or persistent-format flags.

Callbacks still compute their normal logical result. Observed page commits,
root updates and root transactions then discard their proposed mutation during
the scope. Direct observed SET/DEL cleanup requests are accepted as no-ops.
No native preparation, expiry-index update, WATCH invalidation, readiness hint
or mutation observer is published for these discarded changes. Read observers
and connection/statistics operations retain their existing behavior.

GET, TYPE, EXISTS, TTL and similar readers continue to hide expired values.
Legacy-to-page imports and PFCOUNT cache refreshes can be calculated for the
reply without being persisted by a replica read. Such computations may still
allocate transient storage; this is not a zero-allocation read claim.

## RANDOMKEY with retained expired roots

The previous RANDOMKEY loop assumed that rejecting an expired candidate would
physically shrink the database. That assumption is false during replica reads.
The command now retries up to 100 physical candidates and then uses a uniform
reservoir over live root headers under the same frozen exclusive snapshot.
It never builds a full key list. Existing primary cleanup/rank selection remains
unchanged. A fallback scan costs O(stored roots), with a candidate and current
root/key buffer; decoder/native-cache allocations remain separate. This is not a
constant-time worst-case RANDOMKEY claim.

If live roots exist, the fallback selects one without changing any root or
page. If none exist but physical roots remain, RANDOMKEY returns the retained
expired candidate. Redis also permits such an expired name to avoid infinite
retry on a replica; see its
[dbRandomKey implementation](https://github.com/redis/redis/blob/unstable/src/db.c).
GET still reports nil, and KEYS/SCAN still filter logically expired values.
An empty physical database continues to return nil. Empty and binary candidate
names must remain distinct from nil.

The raw-key visitor contract now permits read-only raw root metadata lookups
while exclusive admission is held. Public locking APIs and mutations remain
forbidden inside those callbacks. MDB/oneTBB visit stable index entries without
holding native key locks, RocksDB uses an independent index iterator, and
WiredTiger leases a separate session for the nested root lookup. Workspace
visitation excludes overwritten base entries before emitting staged writes,
so fallback sampling sees each logical owner once.

## Authoritative and explicit writes

Incoming `replica_set`/`replica_del`/`replica_erase` retain their native paths.
Received page transactions use the existing unobserved commit path, and memory
cross-DB groups retain their own native preparation/publication. These trusted
APIs apply even inside a read guard. Their normal local effects remain intact;
ordinary mutation observers are not echoed back into the private replication
stream.

Primary reads retain their normal lazy cleanup/cache behavior. Explicit local
writes on a writable replica are also unchanged, including legacy import on an
actual write. Each executed command gets its own scope, so a read cannot mask
a later write in the same actor or writable EXEC.

Physical retention means data remains until an authoritative deletion or a
subsequent explicit write removes/replaces it. This patch does not implement a
collector for locally originated TTL keys on writable replicas. Redis has a
separate collector for those keys; PKV's existing replica expiry-service policy
is unchanged. Standard PSYNC/backlog/full-sync/relay behavior is also outside
this patch. Suppressing a read-induced mutation is not proof of full Redis
replication compatibility.

## Verification

The C++ fixture captures actual pre-expiry primary mutations, applies the
initial state, then delays later mutations until after replica reads. String,
Hash, List, Set and ZSet cases retain 48-KiB blobs/members (70-KiB String) and
verify original root bytes, page IDs, CRCs and data addresses before replay.
Delayed updates plus PERSIST must recover every unchanged payload page. Other
checks cover WATCH of logically missing data, wake hints, read-only EXEC,
legacy import, HLL cache, primary cleanup and writable replica commands.
Four threads with sibling ACL fibers exercise yield/nesting/exception isolation.
RANDOMKEY cases cover all-expired empty/binary names, mixed live/expired sampling
and staged workspace visibility. Two intermediate sampler issues were recorded:
an unchanged-count error and a nil result that missed Redis's expired-name
exception. Independent probes retain those failures and the final correction.

A real TCP fixture captures three native v3 transactions from a primary,
forwards the initial value through a controlled upstream, waits past the old
deadline, and reads DB 1 on the replica. Markers stay in DB 0 so RANDOMKEY in
DB 1 exercises the all-expired physical population. A downstream subscriber
must receive no read-induced deletion. Forwarding the delayed one-page update and metadata-only
PERSIST must restore the full 70000-byte value. The fixture runs with each
configured backend, not just an in-memory override.

See [scoped validation](benchmarks/retention-validation-2026-11-01.json) for the
exact source/build/runtime/log fingerprints and selected test names. Public
MOVE, its bounded source publication and durable cross-DB replay still need
integration; preserving replica source pages removes one prerequisite bug.

Eight configurations completed all-target builds and 458 selected CTests:
MDB Debug/Release 49 each, oneTBB Debug/Release 57 each, RocksDB Debug/Release
61 each, and WiredTiger Debug/Release 62 each. Four Debug configurations use
ASan/UBSan with the fingerprinted fiber runtime. These are selected regression
tests, not the full CTest suite. WiredTiger configurations ran separately.

The frozen source manifest contains 631 entries: 620 unchanged from the
cross-DB group baseline, eight modified and three added. Native backend, codec,
RPC and replication implementation sources remain byte-identical to that
baseline. Added/changed code lines satisfy the 80-column limit; new functions
and conservative production function-scope bounds stay below 200 lines.

## Primary regression measurements

Retained C++11 Release binaries from the preceding group validation and this
source state used identical flags. The first probe took 60 alternating samples
across MDB/oneTBB GET (RESP2/RESP3), PTTL, overwrite and churn. Write medians
changed by -0.68% to +0.92%; short read samples were more variable.

A separate, nonoverlapping follow-up increased each timed read sample from
131072 to 1048576 operations and used five alternating rounds. Median rate
changes relative to the preceding revision were:

| Backend | GET RESP2 | GET RESP3 | PTTL |
| --- | ---: | ---: | ---: |
| MDB | +0.05% | +0.78% | -2.00% |
| oneTBB | +1.90% | +0.02% | -1.23% |

The short-sample oneTBB RESP2 decline did not recur. PTTL still shows a small
measured cost, retained as an optimization target. Neither run establishes
statistical significance or general throughput improvement. Both use four
Python clients, two server threads, pipeline 256, 64-byte values and loopback;
unrelated machine load is uncontrolled. They compare PKV revisions, with no
official Redis performance claim. Full samples, binary/flag/helper fingerprints
and limits are in the [initial probe](
benchmarks/retention-primary-regression-2026-11-01.json) and
[longer read follow-up](
benchmarks/retention-primary-read-followup-2026-11-01.json).

A subsequent [disabled-guard fast path](READ_GUARD_FAST_PATH.md) moves only
the enabled check inline, preserving the retention contract described above.
