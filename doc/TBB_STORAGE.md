# oneTBB memory storage

`HAS_TBB=ON` enables `mdb_tbb`, including bounded native Hash/List/Set storage,
atomic multi-root/page batches, private page ownership and key SCAN. Build with
the normal oneTBB include/library discovery and choose `mdb_tbb.cf` at startup.
The native formats and command adapters are shared with `mdb`.

## Concurrency and ownership

Each physical shard uses `concurrent_hash_map<string, shared_ptr<record>>`.
Each record contains a root and a page-ID map. Keys and values are binary-safe,
including empty keys. GET and page lookup briefly pin the record through a
`const_accessor`, then release it before copying payloads or yielding. Public
logical-key stripes protect record contents and multi-key command atomicity.
OneTBB 2021.10 uses `spin_rw_mutex` internally; this is not a completely lock-free
execution path.

SCAN uses 256 lazy buckets per shard, with a fiber-aware mutex per bucket and
pointers to canonical keys in stable oneTBB nodes. Insert/delete updates the
index under that mutex, and SCAN copies keys while holding it. It never walks
an unsafe concurrent container iterator or stores a second copy of every key.
Batch topology locks are acquired in shard/bucket order. Existing-key root and
page updates do not acquire index locks. SCAN COUNT remains a hint, since each
bucket is returned whole. Database destruction requires all users to stop.

## Failure atomicity

A batch first prepares owner references, capacity and topology locks. Reset
page maps are detached by swap for rollback. Only touched roots/pages are
copied; existing page maps are never cloned wholesale. Insertions are journaled,
then final root/page swaps and erasures publish without planned allocations.
Failures restore reset maps and remove inserted pages, owners and index entries.
Old payloads are reclaimed after releasing SCAN index locks.

OneTBB can publish a new node before a subsequent segment allocation throws.
The insertion helper retains its accessor through the exception and removes a
partially published owner and index entry. Fault sweeps also retry the same
container after every failed insertion. Raw SET replacement/delete releases
all pages belonging to the old value; replicated root replacement does likewise.

## Validation

C++11 Release and C++17 ASan/UBSan builds pass the full suite with oneTBB enabled
(29 CTests each) and disabled (28 each). Native oneTBB fixtures cover allocation
rollback/retry, multi-root/page transactions, binary keys, concurrent SCAN and
four threads each running four yielding fibers. The five real-server fixtures
select oneTBB in enabled builds, including command compatibility, cluster
routing, replication and output queue limits.

An intermittent replication shutdown SIGBUS was traced to releasing the socket
while a reader fiber still used ACL's event. The writer now shuts down the
socket, waits for reader completion, then closes it. A 100-cycle shutdown
fixture covers pre-start and concurrent disconnects. The complete four-build
matrix passed after this change; this does not prove all races are absent.

## DAO microbenchmark

Measured on the local macOS arm64 host, 2026-10-30, C++11 Release, oneTBB
2021.10.0. Run after builds/tests finished:

```sh
/tmp/pkv-tbb-c11-build/tests/memory_backend_bench 1000 1024
```

Each worker owns a separate Hash containing 1024 payload fields plus `count`
and `probe`. Each iteration executes HINCRBY and HGET, both counted as logical
operations. Twenty warmup iterations per worker are excluded; timing includes
worker completion/join. Three fresh samples per configuration, fixed order.
Every sample verifies the exact final counter and retrieved value. Results:

| Engine | Format | Threads | Median ops/s | Min | Max |
|---|---|---:|---:|---:|---:|
| mdb | legacy JSON | 1 | 12896.5 | 11138.4 | 13203.1 |
| mdb | paged Hash | 1 | 182453.3 | 135376.4 | 186266.0 |
| oneTBB | legacy JSON | 1 | 13004.7 | 12999.6 | 13034.8 |
| oneTBB | paged Hash | 1 | 221115.6 | 216172.4 | 224387.1 |
| mdb | legacy JSON | 4 | 45318.8 | 45039.4 | 45456.5 |
| mdb | paged Hash | 4 | 615443.8 | 614553.4 | 623830.3 |
| oneTBB | legacy JSON | 4 | 45686.3 | 45269.8 | 46014.4 |
| oneTBB | paged Hash | 4 | 713789.2 | 666106.9 | 714004.1 |

Here oneTBB paged Hash reached about 1.21x/1.16x mdb paged Hash at 1/4 threads,
and about 17.0x/15.6x its own legacy JSON path. These short local DAO samples
exclude TCP, RESP, fiber scheduling, replica observers and durability. They do
not establish Redis server throughput superiority, latency or general scaling.
CPU model could not be queried under the sandbox; background load and fixed
configuration order limit reproducibility. Raw CSV is checked in alongside
other benchmark artifacts.

## Remaining scope

RocksDB now provides per-physical-instance page/batch commits; cross-path
mutations remain unsupported. WiredTiger now has per-connection native
transactions; see `WIREDTIGER_STORAGE.md`. Other memory backends still need
native implementations. See `ROCKSDB_STORAGE.md`. Set commands use Map membership pages, and ZSet commands
use coordinated native member/score indexes. Initial replication
sync, replay, durability and broader Redis compatibility remain active work.
