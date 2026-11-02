# Local source EXEC benchmark

These are diagnostic measurements of two local pkv snapshots, not upstream
Redis results. They precede the later legacy TTL/transfer migration integration; the
recorded binary hashes identify the measured implementation. The baseline predates live source workspace routing. The final
binary includes source staging, one source commit, one private workspace mutex,
lazy view-local WATCH metadata and prepared source deltas. The core changes
passed 426 backend-matrix tests. The final legacy eligibility correction was
then rebuilt in all eight configurations and passed 28 affected-path tests
before measurement.

Each run uses a fresh isolated server/database, one TCP client, 32 SET commands
per group, 64-byte values, 20 warm-up groups and three measured samples.
Variants alternate order between samples. EXEC groups include MULTI, QUEUED
responses and EXEC replies; ordinary control groups pipeline SET without MULTI.
The metric counts successful logical SET commands per second, even when repeated
writes coalesce into fewer physical changes. Server startup/teardown is excluded.
No benchmark variants run concurrently, and no build/regression jobs remained
active. The desktop host can still introduce measurement noise.

Host: macOS 26.3.1, ARM64, 10 logical CPUs. Builds are Release, without server
sanitizers. Memory/oneTBB/WiredTiger use C++11; RocksDB uses C++17. Raw JSON records
include binary hashes, host/Python metadata, harness hash, build options and all
sample values. Baselines are local uncommitted-worktree-stage binary snapshots,
not versioned upstream releases. Final content-addressed snapshots are kept
in /tmp and recorded in snapshot_path. Temporary paths can later be replaced;
the recorded SHA-256 identifies the actual measured binary.

| Backend | Mode | Keys/group | Groups/sample | Before SET/s | Final SET/s | Change |
| --- | --- | --- | ---: | ---: | ---: | ---: |
| mdb | exec | 32 keys | 5000 | 452,372 | 405,196 | -10.4% |
| mdb_tbb | exec | 32 keys | 5000 | 449,785 | 402,859 | -10.4% |
| mdb | exec | 1 key | 5000 | 447,240 | 440,554 | -1.5% |
| mdb_tbb | exec | 1 key | 5000 | 445,722 | 439,510 | -1.4% |
| mdb | ordinary | 32 keys | 5000 | 557,922 | 557,332 | -0.1% |
| mdb_tbb | ordinary | 32 keys | 5000 | 555,332 | 552,753 | -0.5% |
| rdb | exec | 32 keys | 1000 | 187,588 | 247,457 | +31.9% |
| wdb | exec | 32 keys | 100 | 242 | 6,475 | +2575.4% |

Memory/oneTBB ordinary controls are within about 1% of baseline in this run.
Their distinct-key EXEC cases still incur about 10% CPU/protocol throughput cost;
repeated-key EXEC is near baseline. This remains optimization work, not a
performance win to generalize to all workloads. RocksDB improves by about 32%
in this local case. WiredTiger benefits strongly from reducing synchronous engine commits
from per-command writes to one commit per EXEC (about 26.8x in this case).

Durability policies differ: the current RocksDB path uses default WriteOptions
without sync, while WiredTiger commits with sync=on. Do not use this table to
rank engines or infer power-loss guarantees. No active replicas, concurrent
clients, large container benchmark, general p99 study or stock Redis comparison
is included.

The first source implementation showed roughly 25% memory EXEC regression in
shorter diagnostic samples. A 3-second native sample identified repeated
construction/destruction of 256 ACL mutexes per workspace. Those diagnostic raw
files remain under source-exec-<backend>-2026-10-31.json; additional intermediate
files are labeled optimized/prepared. They used different sample lengths and
are not a controlled attribution of each optimization's gain. Native profiles
are /tmp/pkv-source-exec-before.sample.txt and
/tmp/pkv-source-exec-after.sample.txt. Final rows above use only the final data.

## Raw final measurements

- [source-exec-mdb-final-2026-10-31.json](benchmarks/source-exec-mdb-final-2026-10-31.json)
- [source-exec-mdb_tbb-final-2026-10-31.json](benchmarks/source-exec-mdb_tbb-final-2026-10-31.json)
- [source-exec-hot-mdb-final-2026-10-31.json](benchmarks/source-exec-hot-mdb-final-2026-10-31.json)
- [source-exec-hot-mdb_tbb-final-2026-10-31.json](benchmarks/source-exec-hot-mdb_tbb-final-2026-10-31.json)
- [source-ordinary-mdb-final-2026-10-31.json](benchmarks/source-ordinary-mdb-final-2026-10-31.json)
- [source-ordinary-mdb_tbb-final-2026-10-31.json](benchmarks/source-ordinary-mdb_tbb-final-2026-10-31.json)
- [source-exec-rdb-final-2026-10-31.json](benchmarks/source-exec-rdb-final-2026-10-31.json)
- [source-exec-wdb-final-2026-10-31.json](benchmarks/source-exec-wdb-final-2026-10-31.json)

Reproduce with tests/source_exec_bench.py, the recorded before/after binaries,
backend/mode/keyspace/groups options and an otherwise idle host. Keep binary
identity and engine durability settings constant when comparing variants.
