# Blocking sorted sets

PKV supports BZPOPMIN, BZPOPMAX and BZMPOP on MDB, oneTBB, RocksDB and
WiredTiger. These commands use the existing native ZSet member/score indexes
and chunked member payloads. Roots and pages remain bounded at 16 KiB;
40,000-byte members do not become a serialized collection blob.

## Behavior

BZPOPMIN/BZPOPMAX select the first available key in request order, then its
lowest/highest score member. BZMPOP accepts MIN/MAX and an optional positive
COUNT, returning members from one selected key. Equal scores retain binary
member ordering. RESP2 emits bulk-string scores; RESP3 emits doubles. Missing
results and timeouts use the reference null-array/RESP3-null representation.
EXEC runs all three commands without waiting.

The commands share List blocking's parser, deadline, execution suspension,
connection input and cancellation implementation. Decimal timeouts round up
to milliseconds and retain one steady-clock deadline through internal wakes.
CLIENT UNBLOCK, CLIENT KILL, peer disconnect, queued input, ACL changes and
replica write policy use the existing blocked-client control path. Actual
wakes recheck ACL and replica admission before modifying storage.

Each DB has one readiness registry with FIFO heads filtered by waiting type.
List and ZSet requests share overlapping-key reservations and producer order.
A ZSet producer cannot wake a List waiter on the same key, and vice versa.
SET, deletion, expiry and unsuccessful commits do not create producer
readiness. Preparation happens before committing storage; failed preparation
or a failed native commit publishes nothing.

Ordered EXEC/private RPC observations preserve producer order. A temporary
List producer can establish a key's priority even when its final type is a
ZSet, provided some List client was blocked when that producer executed.
The reference's blocked-type condition is global across logical DBs. Local
workspace observations retain that condition at mutation time; cancelling a
waiter later inside EXEC cannot rewrite prior producer eligibility. Private
RPC replay uses receiver-side waiting types.

All EXEC paths defer readiness until their end, including transactions that
use CONFIG or span DBs and therefore bypass the single-DB workspace. An
actor-owned batch allocates its event slots before each native write, records
only successful commits, then publishes final states without C++ allocation.
Later command failure preserves earlier committed events. The batch belongs
to the executing fiber and cannot intercept another fiber's notifications.

An actual wake reserves overlapping request keys, then re-executes the original key order.
The trigger key and final pop key may differ. If an earlier requested key has
become a wrong type when another key produces a real wake, re-execution can
return WRONGTYPE while preserving the producer's value.

Cancellation promotion is a probe hint, not a real producer. A blocked client
must continue waiting if a preceding client is cancelled after the key changes
to a string. The shared implementation now suppresses WRONGTYPE from this
probe-only path. A new differential regression covers both BLPOP and BZPOPMIN;
immediate probes and actual producer re-execution still report wrong types.

## Producer and storage coverage

The protocol suite exercises ZADD, ZINCRBY, ZUNIONSTORE, ZINTERSTORE,
ZDIFFSTORE, ZRANGESTORE, COPY, RENAME, RENAMENX, GEOADD, GEOSEARCHSTORE,
GEORADIUS STORE and GEORADIUSBYMEMBER STOREDIST as producers. Cross-DB COPY
checks publication on the destination DB. MOVE remains unsupported and is
outside that cross-DB coverage.

Native tests exercise ordered workspace commits and grouped page replay on
each backend, type filtering, independent List/ZSet subscriptions, WATCH
invalidation and bounded stored records. A 512-budget allocation sweep with
both subscription types active and deferred publication checks rollback:
failed writes publish no event, leave the key absent and preserve WATCH.
A further 128 budgets check that a later failed write does not discard an
earlier successful notification. Final publication runs with C++ allocation
disabled. Native tests also check batch ownership across two fibers and
4096-key mixed-type grants. The underlying allocation-free queue dispatch and four-thread/sixteen-fiber
handoff remain covered by the shared waiting tests.

## Validation and remaining scope

New protocol checks include 288 stable differential observations and 18 typed
RESP2/RESP3 observations, plus sixteen FIFO consumers, overlapping requests,
wrong-type cancellation, mixed List/ZSet waiters and producer order, global
blocked types, CONFIG/cancellation inside EXEC, producer commands, pipeline
ordering, 40,000-byte members, ACL revocation, replica policy changes, WATCH,
DB isolation, cluster errors, disk restart and grouped private RPC replay.
Correctness uses the local modified Redis 8.6.2 reference.

The preceding blocking-ZSet revision passed an eight-configuration matrix
of 1008/1008 tests: MDB Debug/C++11
114 each, oneTBB Debug/C++11 126 each, RocksDB Debug/Release 132 each and
WiredTiger Debug/C++11 132 each. Debug builds include ASan/UBSan. All 181
recorded source fingerprints remained fixed through the final matrix and
measurements. Benchmark binaries and flags match the full-matrix builds.
Fresh cancellation-isolation probes passed in C++11 and ASan/UBSan builds.

Independent probes reproduced the previous List cancellation bug and the
cross-type ordering mismatch in the separate-registry implementation draft.
The final version matches the reference. A separate probe also checks EXEC
producers spanning DB 0 and DB 3, in both List/ZSet production orders.

Matched hot inline GET median changes relative to the preceding List revision:

| Backend | RESP2 | RESP3 |
| --- | ---: | ---: |
| MDB | +0.458% | +0.098% |
| oneTBB | -0.836% | +0.251% |

Each backend checked 50,331,648 replies across three rotating samples per
variant. These small differences do not establish statistical significance.

Final configuration logs, source fingerprints and matched hot inline GET
measurements are recorded in [the validation report](
benchmarks/zblocking-validation-2026-11-01.json). GET compares PKV revisions;
it does not establish a speed advantage over stock Redis, mixed workloads,
paged reads or disk workloads. Measurements use three rotating samples per
variant on macOS ARM64; Linux/GCC and statistical significance are unverified.

Blocking pop commands for Lists and ZSets are implemented. Streams, Pub/Sub,
scripting, complete Redis replication and other server gaps remain required
work. Full Redis compatibility remains an active goal. See also the shared
[List blocking contract](BLOCKING.md).

The current registry's per-type indexes, allocation ownership checks and
validation are documented in [queue scaling](QUEUE_SCALING.md).
