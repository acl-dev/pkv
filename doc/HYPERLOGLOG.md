# HyperLogLog

PKV implements PFADD, PFCOUNT and PFMERGE with Redis-compatible binary String
values. These commands use the native paged String API and the same atomic page
transactions as other String operations. They do not serialize sketches as JSON
or store individual input elements.

## Representation and bounds

The logical String uses the HYLL header, 16,384 six-bit registers, the
endian-neutral MurmurHash64A hash with Redis's seed, and Ertl's sigma/tau
cardinality estimator. Empty sketches use the compact 18-byte sparse form.
Normal sparse updates promote at the default 3,000-byte threshold or when a
register exceeds 32. Dense Strings occupy 12,304 bytes,
including the header. The runtime hll-sparse-max-bytes setting is not yet
implemented; this increment uses the Redis default threshold.

Registers remain an approximate representation of a set. A newly observed
element can leave every register unchanged, so PFADD can return zero even for
a distinct input. Binary elements and binary keys are supported. TYPE returns
string; GET, SET, COPY, String bit operations and persistence retain the normal
String interoperability contract.

Standard Redis sparse and dense dumps can be imported with SET and returned
with GET. Valid noncanonical sparse inputs can use up to 32,784 bytes: each
opcode occupies at most two bytes and covers at least one of 16,384 registers.
The native storage representation still splits these into pages of at most
16 KiB. Normal generated dense sketches need one private data page; small
sparse sketches fit inline in the bounded PSR root. This adds no storage format
version or new value-kind tag, so existing paged String readers can read them.

Native reads inspect the 16-byte header before loading the sketch. A valid
single-key cached PFCOUNT and an existing-key PFADD without elements need no
body read. Operations that need registers reject an oversized sparse body
before materializing it. An injected native test uses a 4 MiB fake sparse
String and caps individual C++ allocations at 64 KiB: the five checked paths
each read just the first native page. Legacy JSON roots still incur their
existing codec/import costs; the native read bound is not a promise about
legacy decoding.

## Mutation, cache and transactions

PFADD creates an empty sketch when missing, returns one for creation or a
register change, preserves TTL and invalidates the cached cardinality only
when changed. Repeated elements without a register change leave WATCH valid.

Single-key PFCOUNT returns a valid cached cardinality without changing data.
An invalid cache is recomputed and updated in the same native transaction.
That internal cache write preserves TTL and invalidates WATCH, as in the
tested Redis reference. ACL authorization still requires only read-key
access. Multi-key PFCOUNT computes register maxima with bounded scratch memory
and does not update source caches.

PFMERGE includes the old destination among its inputs. It validates sources
before changing the destination, preserves an existing destination's TTL,
creates an empty destination for absent inputs and invalidates the cache and
WATCH even for an otherwise identical result. Expired destinations are
recreated without their old TTL. Native allocation failures or a rejected
storage commit leave destination roots, pages and WATCH state unchanged.

The commands participate in MULTI/EXEC, ACL selectors, command metadata,
cluster slot checks, logical DB selection and the existing private RPC page
stream. Tests cover a replica's local cache update followed by primary writes
and a grouped EXEC containing PFADD and PFMERGE. This does not add Redis PSYNC,
initial synchronization or other missing replication features.

The shared read-only String helper now respects staged root deletion/write
actions. This fixes duplicate expired keys in PFCOUNT/PFMERGE. INFO sections
also remain outside the transaction key set, fixing a prior routing edge case.
COMMAND INFO now retains optional key-specification notes; the existing
metadata test compares those notes rather than removing them from its oracle.

## Validation scope

The socket fixture compares 191 stable observations against the local modified
Redis 8.6.2 reference, including exact sparse/dense bytes, cache transitions,
imported sketches, malformed data, saturated dense registers, binary keys,
logical DBs and complete command descriptors. Additional checks cover TTL,
WATCH, read/write ACL patterns, cluster cross-slot rejection, keyless INFO in
EXEC, eight concurrent writers, disk restart and private RPC updates.

Native tests cover every dense register position, randomized six-bit values,
sparse promotion,
bounded input reads, legacy JSON migration and source preservation, failed
storage commits, and 128 allocation budgets for each of PFADD, PFCOUNT and
PFMERGE. oneTBB builds run the native allocation checks against both memory
backends. Debug sanitizer configurations remain part of the validation matrix.

The final revision passed 878/878 CTests on macOS ARM64: memory/C++11 memory
100 each, oneTBB/C++11 oneTBB 109 each, and RocksDB Debug/Release plus
WiredTiger Debug/C++11 115 each. Debug builds used ASan/UBSan. All 106 source
fingerprints stayed fixed through the matrix; original test timeouts,
concurrency checks and disk synchronization settings were retained.

GET regression measurements used identical C++11 Release flags within each
backend, two server threads, four clients, 64-byte values, 256-command
pipelines, 4096 batches per worker, three rotating rounds and disabled active
expiration. All 100,663,296 measured responses were checked. The same-protocol
median changes against the prior dispatch revision were:

| Backend | RESP2 | RESP3 |
| --- | ---: | ---: |
| MDB | -0.52% | -1.43% |
| oneTBB | +1.44% | +2.76% |

These local hot-key measurements do not establish statistical significance or
HLL-operation, mixed-workload, disk or stock Redis throughput. The benchmarked
binaries match the binaries used by the final validation matrix. See
[validation evidence](benchmarks/hll-validation-2026-11-01.json),
[MDB raw GET results](benchmarks/hll-get-mdb-2026-11-01.json) and
[oneTBB raw GET results](benchmarks/hll-get-tbb-2026-11-01.json).

PFDEBUG/PFSELFTEST, the full Redis command inventory, Redis RDB/AOF, blocking
operations, Pub/Sub, scripting and complete cluster/replication behavior remain
ongoing work. Native multi-path disk transaction limitations still apply.

Interface references: [PFADD](https://redis.io/docs/latest/commands/pfadd/),
[PFCOUNT](https://redis.io/docs/latest/commands/pfcount/) and
[PFMERGE](https://redis.io/docs/latest/commands/pfmerge/). The estimator follows
[Otmar Ertl's cardinality-estimation paper](https://arxiv.org/abs/1702.01284).
