# Dense memory key indexes

MDB and oneTBB now select a physical stored root by ordinal using arrays of
borrowed stable hash-node pointers. After selecting the shard and, for oneTBB,
the scan-index bucket, selection accesses the requested array position directly.
The previous walk within a selected collision bucket is removed. With fixed
shard/bucket counts, selection work no longer grows with that bucket's key
population; copying the selected key still depends on its length.

## Storage and synchronization

The array owns neither key strings nor values. Each root node carries a mutable
position, protected by the same existing lock as its array. Hash rehashing keeps
node addresses stable; array growth moves pointers rather than root objects.
MDB uses one array per shard under its existing shard lock. oneTBB uses its
existing lazily created scan-index buckets and their fiber mutexes, retaining
independent write concurrency between buckets. No global write-index mutex is
added. Normal GET reads the record pointer/value without accessing positions.

Deletion swaps the final pointer into the removed position, updates that node's
position, then removes the final slot before destroying the root node. It never
allocates. Consequently SCAN order can change after deletion; SCAN does not
promise stable ordering. Logical key locking and the existing scan locks keep
borrowed nodes alive while being accessed.

Insertion reserves a private pending slot before allocating a hash node, then
publishes the node and its position. Failure cancels that slot. Batch preparation
records inserted roots; rollback removes them through the same swap-last path,
so positions of other moved nodes remain valid. oneTBB also removes an owner
node if a partially completed hash insertion throws after acquiring it. No
pending slot becomes visible outside the corresponding index lock.

This changes only in-memory root metadata. Native paged String, List, Hash, Set
and ZSet layouts, codec bytes, disk storage and transaction command semantics
remain unchanged. Collection values are still bounded pages rather than one
serialized collection blob.

## Memory and limits

MDB adds a position field per root and a pointer array per shard. oneTBB replaces
its scan hash-set entries with root positions and pointer arrays. Both arrays
can have spare capacity, and nonempty arrays can retain a previous high-water
capacity after deletion or failed preparation. Empty arrays with at most eight
slots are cached for repeated small-key churn; larger empty arrays are released
without allocating. Logical rollback does not promise exact allocator/RSS
rollback.

RANDOMKEY still holds node-wide exclusive execution admission during selection
and passive expiry. RocksDB, WiredTiger and staged workspaces still stream key
names to the selected rank. Their cost can grow with database size, and long
selection/cleanup can delay other databases. This index does not resolve those
remaining bottlenecks or complete Redis compatibility.

## Verification and measurement

The `dense_index` fixture compares every deterministic rank and the physical
root count with SCAN membership after growth, deletion and reinsertion. It uses
96 keys forced into one collision bucket, checks failed and successful direct
and transaction writes across 192 allocation budgets per memory backend, and
runs concurrent colliding writers alongside SCAN and selection. A synthetic
index fixture checks cancellation, allocation-free arbitrary deletion and
repeated small empty/nonempty reuse.

Measurements compare retained parent and candidate C++11 Release binaries with
identical compiler flags. Raw index measurements use the same benchmark source
and compare native/visitor sequence checksums within each binary; processes
have independent random seeds. GET and write benchmarks verify every reply.
The write cases cover overwriting hot roots, inserting/deleting roots, and
retaining new roots, with resident memory sampled only from the owned server
process. RSS includes the runtime, allocator and caches, so it does not measure
exact index bytes. Short rotating runs do not establish statistical significance
or throughput against official Redis.

The final eight-configuration matrix passed **1,086 CTests**, with four focused
fixtures each passing five additional repetitions. Four ASan/UBSan builds use
the separately identified ACL fiber runtime with sanitizer stack registration;
the C++11 Release comparisons use the original installed runtime. No TSan run
is claimed. All 569 frozen source fingerprints and unchanged codec/disk/DAO
sources are recorded in the [validation report](
benchmarks/dense-validation-2026-11-01.json). The parent reports remain intact.

The [raw index comparison](benchmarks/dense-index-2026-11-01.json) checked
221,184 selections across 216 rows. Medians below use nine internal samples
from three independent processes per variant:

| Backend | Roots | Parent µs/select | Dense µs/select | Parent/dense |
| --- | ---: | ---: | ---: | ---: |
| mdb | 1,024 | 0.144 | 0.078 | 1.85× |
| mdb | 16,384 | 0.186 | 0.092 | 2.01× |
| mdb | 131,072 | 0.597 | 0.124 | 4.82× |
| tbb | 1,024 | 0.148 | 0.145 | 1.02× |
| tbb | 16,384 | 0.201 | 0.145 | 1.39× |
| tbb | 131,072 | 0.675 | 0.177 | 3.82× |

Matched GET comparisons checked 100,663,296 replies in total. Median deltas
against the retained sampler revision were:

| Backend | RESP2 | RESP3 |
| --- | ---: | ---: |
| mdb | -0.25% | -0.85% |
| tbb | +0.68% | -0.15% |

The [MDB](benchmarks/dense-updates-mdb-2026-11-01.json) and
[oneTBB](benchmarks/dense-updates-tbb-2026-11-01.json) write comparisons checked
50,331,648 timed operations in total. Each fill sample retains 1,048,576 roots;
churn leaves zero roots and overwrite retains four. RSS is the median resident
size after the workload, in MiB, rather than index-only allocation:

| Backend | Workload | Ops/sec delta | Parent RSS MiB | Dense RSS MiB |
| --- | --- | ---: | ---: | ---: |
| mdb | overwrite | -1.10% | 5.22 | 5.19 |
| mdb | churn | +1.05% | 5.58 | 5.53 |
| mdb | fill | -2.59% | 211.91 | 241.84 |
| tbb | overwrite | +2.23% | 5.36 | 5.41 |
| tbb | churn | -1.32% | 8.39 | 8.42 |
| tbb | fill | +19.12% | 367.59 | 329.58 |

These local short runs have workload and measurement noise. Differences do not
prove general performance improvements, statistical significance or superiority
over official Redis. In particular, a faster raw index does not remove command
admission, parsing, allocation, network or expiry costs. The previous unexplained
RocksDB Release startup SIGSEGV remains a historical unresolved limitation;
this passing matrix does not establish its cause or resolution.

The [current command inventory](
benchmarks/dense-command-inventory-2026-11-01.json) still reports 230 PKV names
against 412 local-reference names, with 183 missing names and the extra
cluster|addslave alias. The isolated reference is locally modified Redis 8.6.2.
Name presence does not establish semantic parity. Full Redis compatibility,
standard replication, Streams, scripting, atomic cross-DB MOVE and broader
administration remain unfinished.
