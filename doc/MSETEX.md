# Atomic multi-key String assignment with expiry

MSETEX is implemented using native page transactions. Its syntax is:

```text
MSETEX numkeys key value [key value ...] [NX | XX]
  [EX seconds | PX milliseconds | EXAT seconds | PXAT milliseconds | KEEPTTL]
```

The reference is the [Redis MSETEX documentation](
https://redis.io/docs/latest/commands/msetex/) and the local modified Redis 8.6.2
binary/source. The command returns integer 1 when the whole assignment applies,
or 0 when a condition fails. NX requires every selected key to be absent; XX
requires every selected key to exist. Expired keys count as absent. Existing
values of any supported type may be overwritten as Strings.

A shared expiry is computed once for the command. KEEPTTL instead preserves
each live key's individual deadline; missing/expired keys become persistent.
Without an expiry option, successful updates clear previous TTLs. Duplicate
keys use the last value, and conditions inspect the original key population
before any replacement. Repeated identical options are accepted with the last
expiry argument winning. Conflicting conditions/expiry modes are syntax errors.

## Parsing, routing and observability

`numkeys` is a canonical positive integer through INT_MAX, checked against the
actual available pair count before multiplying or allocating plans. Values and
keys are binary safe, including empty keys and NULs. Keys/values spelling NX,
EX or other option names remain data inside the counted pair region. Shared
expiry arguments are validated before any lookup or mutation.

Option names require their complete byte length. NUL-suffixed option tokens
are rejected rather than interpreted as C-string prefixes. Relative expiration
arithmetic rejects signed overflow and preserves the old batch. The local
reference accepts an extreme relative PX duration after signed overflow; that
boundary is a separate strictness assertion, as with existing SET/GETEX guards.
It is not counted as an identical-reply differential case.

Cluster routing and EXEC key extraction consume exactly the counted keys at
stride two. The count, values and option tail are not routed as keys. Invalid
counts reach normal command validation; distinct valid key slots yield
CROSSSLOT. COMMAND metadata and argument-only GETKEYS/GETKEYSANDFLAGS include
MSETEX and retain duplicate key arguments in discovery replies.

Successful writes invalidate WATCH and publish the normal native root/page
batch. Failed live conditions preserve values, individual TTLs and WATCH state.
An absolute past deadline is stored as supplied, making values logically absent;
physical expired roots/pages can remain until lazy or active cleanup. This
matches the existing physical-root DBSIZE convention. Active expiry tracks all
committed deadlines and replicates physical removal to passive receivers.

## Atomicity and stored representation

Native execution locks the complete unique key set, checks conditions first,
stages replacements and commits once. Clients cannot observe a partially
replaced batch or mixed values through a coherent MGET. Eligible source EXEC
uses its staged view and final source batch; subsequent commands see the new
values and per-key TTLs before commit replies are published.

The input plan owns keys but borrows immutable request value pointers for the
synchronous call. It does not duplicate every full input String. Each result
uses its own bounded PSR root and 16-KiB private pages, never one serialized
multi-key aggregate. Workspace staging owns copied page bytes before the
borrowed request arguments go away. The plan/transaction still have metadata
cost proportional to keys/pages, and the RESP request itself remains owned by
the existing parser. This is not a constant-total-memory or wall-time bound.

Liveness/TTL checks use stored-header inspection, avoiding native payload reads.
Legacy roots retain codec parsing cost; successful native writes replace them
with paged Strings. A failed condition leaves live legacy bytes untouched.
The legacy JSON DAO implements the same batch API for nonpage integrations;
its existing materializing/serialized storage costs remain. Default built-in
server backends use the native implementation.

MDB and oneTBB support native memory batches across their stripes. RocksDB and
WiredTiger support one atomic engine transaction when all changed owners map
to one physical instance. Multi-instance disk writes remain unsupported before
any change; this command does not add a durable cross-engine coordinator.
A read-only failed condition can succeed as a no-op across those instances.
Use the existing single-instance native disk configurations for atomic batches.
Allocation failures, rejected commits and unsupported physical groups cannot
leave a subset of replacements committed.

No stored-format version, replication version, dependency or configuration
default changes are required. Existing durability policies, bounded replication
queues and missing initial-sync/durable-replay guarantees remain unchanged.

## Verification

C++ tests cover shared/preserved/past deadlines, NX/XX whole-batch behavior,
legacy JSON integration, concurrent MGET coherence, rejected commits and
allocation-failure rollback over native/legacy inputs. Two 1-MiB values commit
under a 64-KiB per-allocation ceiling, with one native apply and bounded pages.
oneTBB builds repeat memory cases on both engines. Backend fixtures separately
reopen disk data to check payloads and deadlines; cross-instance attempts must
leave all old bytes/TTLs intact, while NX no-ops remain successful.

TCP differential checks cover populations and non-String values, binary and
empty inputs, repeated keys/options, invalid counts, malformed options and
expiry errors, past deadlines, logical DBs, WATCH, source EXEC and command-key
metadata. Independent checks cover relative deadline bounds, strict tokens /
overflow, one-winner NX races and cluster counted-key routing. A primary /
receiver fixture checks coherent multi-key delivery, deadline preservation,
source EXEC and background physical expiry using DBSIZE without reading the
expired keys. It first establishes live RPC readiness; initial synchronization
and durable replay are not inferred.

These tests do not establish complete Redis compatibility, crash safety,
general race freedom or server throughput superiority. Physical multi-instance
atomic recovery and other remaining Redis commands stay part of the goal.


The final matrix passed 632/632 CTests across eight C++11/C++17 memory, oneTBB,
RocksDB and WiredTiger configurations, with 499 common protocol comparisons per
build. The first six-way parallel run encountered system connect-address
errors; failed memory cases passed in isolation, and full suites then passed
with at most two configurations running together. Individual test concurrency,
expectations, timeouts and durability settings were unchanged. All final
sessions reached terminal success. See the
[test history](../tests/README.md#atomic-msetex) and
[validation identities](benchmarks/msetex-validation-2026-10-31.json).
