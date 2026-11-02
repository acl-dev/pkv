# RANDOMKEY and TIME

RANDOMKEY returns one live key from the selected logical database, or a null
reply when no live root remains. Empty and arbitrary binary key names are
supported, as are all five existing value types. Native private pages are never
candidates. The command enumerates the local database in cluster mode without
routing a key or requiring an assigned slot. TIME returns two bulk strings:
Unix seconds and the microsecond remainder, under RESP2 and RESP3.

Both commands have their reference command metadata, ACL categories and arity.
They have no key arguments. Permission to execute RANDOMKEY grants key-name
discovery, independently of GET/key-pattern permissions, as with SCAN and KEYS.
RESP2 subscription restrictions still apply; RESP3 subscribers can use both
commands. Read-only replica admission allows them. This is not a claim of full
standard Redis replication or replicated-expiry semantics.

## Selection and expiry

Each sampling operation holds exclusive execution admission, so the stored-root
count and selected rank refer to one write boundary. It chooses an unbiased
integer in the stored-root population, then maps that ordinal to a key. The
native memory implementations skip whole shards and scan-index buckets by
count, then access a dense array position directly. The arrays borrow stable
hash nodes and add no duplicate key-name storage or KV payload lookup. Root
insertion/deletion maintains positions; see [dense indexes](DENSE_KEY_INDEX.md).
A warmed sampler with a reserved result string needs no C++ heap allocation.

The DAO checks only the selected record header. A live native value is not
materialized or rewritten, and its payload pages are not read. Old legacy
formats retain the cost of their existing metadata decoder. Invalid or
unsupported selected headers return the existing explicit error. Sampling a
live key preserves its TTL and WATCH state.

Expired candidates are removed through the ordinary atomic root-deletion path,
including private-page reclamation and WATCH invalidation, and selection is
retried. Rejecting uniformly selected expired roots preserves equal probability
among the remaining live roots. Cleanup failure reports an error with empty
output; it does not claim a successful selection. A stale backend index that
repeatedly fails to reduce the physical count reports an error rather than
looping forever. Passive expiry may legitimately remove earlier expired roots
before a later storage or allocation error.

RANDOMKEY in EXEC reads the current staged workspace. Base keys shadowed by
staged writes or deletes are excluded; staged writes appear once. Overwriting
a source key therefore cannot increase its sampling weight. A discarded or
failed workspace does not publish those staged changes to the source database.
Multi-DB transactions retain the existing exclusive sequential fallback.

## Time snapshots

TIME reads wall-clock microseconds. An outer exclusive admission captures one
microsecond snapshot and derives its existing millisecond expiry snapshot from
that same value. Nested operations preserve both. Repeated TIME commands in
one EXEC therefore agree, matching the local reference's cached command clock.
Ordinary shared-admission operations retain the previous millisecond clock
path. Wall time can move backward if the operating system adjusts it; this is
not a monotonic timer. Microsecond output resolution does not promise that
individual clock reads have microsecond accuracy.

## Cost and remaining optimization

The memory fast path costs O(shards + scan buckets + selected-key length),
with one owned output key. A selected collision bucket no longer requires a
linear walk. Normal GET and existing-root overwrite do not update the index;
root insertion/deletion maintains the pointer array and per-root position.
MDB gains metadata storage; oneTBB replaces its scan hash-set storage.

RocksDB and WiredTiger stream their existing key-only scan indexes, stopping
at the selected rank. Transaction workspaces stream the merged key-name view.
These paths can take O(database keys), with memory proportional to individual
key lengths rather than the complete key set or collection payloads. Storage
caches, cursor/session allocations and existing staged transaction data are
separate costs. A fully expired disk keyspace can require repeated scans while
reclaiming candidates. Faster disk rank selection remains optimization work
toward the full Redis performance goal.
RANDOMKEY holds node-wide exclusive admission during selection and cleanup, so
long scans can delay other logical databases.

## Verification

`key_sampling` proves rank/population bijections for empty/binary keys,
overwrite/delete, workspace shadowing and deliberately colliding memory keys.
It checks concurrent atomic transfers, no-allocation native selection, a
2-MiB opaque root that selection must not copy, a 2-MiB paged String whose
payload must not be read, 96 allocation budgets, stale/error indexes and
expired-page cleanup, including a failed deletion that preserves pages and
publishes no physical WATCH invalidation.
Backend fixtures exercise one/multiple storage instances and independent
process reopens. The command fixture checks typed replies, all native value
types, lazy expiry, ACL discovery, Pub/Sub, cluster admission, read-only
admission, concurrent writers and cached EXEC TIME. The execution-gate fixture
also checks frozen microsecond/millisecond consistency and clock release.

The original RANDOMKEY/TIME revision passed 1,078 CTests across eight MDB, oneTBB, RocksDB and
WiredTiger configurations, including C++11 and ASan/UBSan. Four focused fixtures
each passed five further repetitions. Each configuration covers 636 behavioral
checks, 96 sampling allocation budgets and the existing 327 typed RESP3
observations. Source/binary/runtime hashes, 565 frozen source files and the
unchanged codec/storage checks are recorded in [the validation report](
benchmarks/sampling-validation-2026-11-01.json). The isolated reference is a
locally modified Redis 8.6.2, not an unmodified official benchmark baseline.

The [raw-key microbenchmark](benchmarks/sampling-index-2026-11-01.json) uses four
memory shards and the same backend/population for both selectors. Paired runs
reset the same random-engine state and compare returned-key sequence checksums.
Three rotating rounds at each size checked 36,864 selections in total. Median
microseconds per selection were:

| Backend | Roots | Native | Streaming visitor |
| --- | ---: | ---: | ---: |
| MDB | 1,024 | 0.144 | 0.879 |
| MDB | 16,384 | 0.182 | 31.162 |
| MDB | 131,072 | 0.568 | 587.145 |
| oneTBB | 1,024 | 0.147 | 0.873 |
| oneTBB | 16,384 | 0.235 | 31.230 |
| oneTBB | 131,072 | 0.594 | 451.949 |

These measurements exclude command decoding, expiry/header validation, network
IO and payload reads. They compare two paths in the same PKV backend, rather
than measuring server throughput or official Redis. They also show increasing
native cost with population size in that revision. The subsequent
[dense-index revision](DENSE_KEY_INDEX.md) removes its bucket walk. Three short
rounds do not establish statistical significance.

The separate matched-protocol GET comparison checked 100,663,296 replies in
three rotating rounds per backend against the retained Pub/Sub revision. Both
use identical C++11 Release core flags, the original ACL runtime, two server
threads, four clients, 256-request batches, one 64-byte hot value and disabled
active expiration. Median changes were +0.19% RESP2/-0.42% RESP3 for MDB and
+0.04%/-0.26% for oneTBB. These small differences do not establish significance
or a throughput advantage over official Redis. Raw samples and fingerprints
are in [MDB](benchmarks/sampling-get-mdb-2026-11-01.json) and
[oneTBB](benchmarks/sampling-get-tbb-2026-11-01.json) artifacts.

The updated [command inventory](
benchmarks/sampling-command-inventory-2026-11-01.json) identifies 230 advertised
PKV names versus 412 local-reference names, including subcommands: 183 reference
names remain missing and cluster|addslave is an extra alias. Name presence does
not prove semantic parity. Atomic cross-DB MOVE, broader administration,
Streams, scripting, standard replication and cross-node propagation remain
pending; full Redis compatibility and performance remain the active goal.
