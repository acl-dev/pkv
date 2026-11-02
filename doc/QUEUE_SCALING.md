# Waiting queue scaling and allocation ownership

The waiting registry now caches one FIFO head and tail per waiting type.
Subscription nodes link into both the original key queue and their type's
queue. Looking up a type head, removing a middle node and promoting type
heads no longer walks every subscriber on the key. Dispatch still considers
all candidate request keys and reserves overlapping requests together.
Generic waits, Lists and ZSets retain their existing ordering and cancellation
rules; the native storage formats and 16 KiB root/page bounds are unchanged.

A newly allocated subscription node now enters a local unique_ptr before
its owner vector grows. If that growth throws, both the node and its owned
key are freed. Registration still happens only after construction succeeds.
Existing key entries are found before attempting to insert a new entry.

## Ownership validation

The heap fixture counts outstanding C++ allocations across 1536 failure
budgets: three waiting types, with and without an existing subscriber. It
checks both active membership and global type counts after each attempt.
Using the same fixture, the preceding implementation leaked 48 allocations
in 30 cases; the corrected version leaked none. This is C++ allocation
accounting, not a claim about every allocation inside third-party C libraries.

The type-index fixture exercises binary and duplicate keys, arbitrary middle
removals, every type's FIFO head, channel reuse, overlapping requests and
publication/cancellation with C++ allocation disabled. Existing thread/fiber,
4096-key, storage fault and blocking protocol tests remain in the matrix.

## Native queue measurements

The standalone key_wait_bench measures queue registration, publication,
handoff and cancellation without storage or sockets. Before/after versions
use the same source and C++11 Release flags; old/new runs alternate across
three repetitions at 512, 2048 and 8192 subscribers per group. Mixed cases
have two groups, hence 16384 subscribers at the largest size. The source,
commands, binary hashes, all 90 observations and medians are retained in
[the native measurement record](benchmarks/queue-native-2026-11-01.json).

The reported operation time combines publication, draining the selected type
and cleaning up the remaining subscriptions. Registration is timed separately.
Publication/draining/cleanup reject C++ allocations. These measurements do
not represent Redis command throughput, thread contention or backend I/O.

At 8192 subscribers per group, the operation-time medians were:

| Case | Before (ms) | After (ms) | Ratio |
| --- | ---: | ---: | ---: |
| List | 138.806 | 1.384 | 100.3x |
| ZSet | 136.502 | 1.393 | 98.0x |
| Grouped List/ZSet | 954.001 | 2.821 | 338.1x |
| Alternating List/ZSet | 696.099 | 2.968 | 234.5x |
| Cancellation | 135.051 | 1.073 | 125.9x |

Caching adds two pointers per subscription/key node and head/tail arrays per
actively waited key. On the tested 64-bit build this is 16 and 48 bytes,
respectively, before allocator overhead. Allocation traffic is reported
separately from retained memory. Stored keys with no subscribers acquire no
such entries. This trades small bounded metadata for avoiding repeated
subscriber scans.

The final eight-configuration matrix passed 1024/1024 tests: MDB Debug/C++11
116 each, oneTBB Debug/C++11 128 each, RocksDB Debug/Release 134 each and
WiredTiger Debug/C++11 134 each. Debug builds include ASan/UBSan. All 184
recorded source fingerprints remained fixed. The measured core archive and
GET executables match the final matrix builds; fresh cancellation and
protocol probes also passed.

Matched hot inline GET median changes relative to the preceding revision:

| Backend | RESP2 | RESP3 |
| --- | ---: | ---: |
| MDB | +1.439% | +1.265% |
| oneTBB | +2.229% | +1.862% |

Each backend checked 50,331,648 replies across three rotating samples per
variant. These small differences do not establish statistical significance
or a causal improvement to GET; ordinary GET does not use the waiting index.

Final matrix and matched hot inline GET measurements are recorded in
[the validation report](benchmarks/queue-validation-2026-11-01.json).
The platform is macOS ARM64. Three samples do not establish statistical
significance. Linux/GCC, network blocking throughput, mixed write workloads
and disk performance have not been benchmarked by this increment.

Full Redis compatibility remains an active goal; this increment improves
the shared waiting layer used by Lists and ZSets. See [blocking lists](
BLOCKING.md) and [blocking sorted sets](BLOCKING_ZSET.md) for command scope.
