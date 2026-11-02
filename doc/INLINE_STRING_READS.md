# Inline String reads

MDB and oneTBB GET can read a valid PSR v1 inline String directly from its
stored root. They retain execution admission and the ordinary logical key
mutex. MDB also holds its shard lock while copying the payload. oneTBB obtains
the record with its concurrent hash map and keeps the logical key lock while
reading the record bytes, matching its existing writer synchronization.

The path validates the complete root header, version, flags, length and page
count before copying at most 1024 payload bytes. A missing record returns a
missing result. A preallocated output buffer allows these reads to finish
without heap allocation. Output allocation failure returns a storage error
without changing the record or returning a partial value.

Expired, paged, legacy, malformed, unsupported-version and non-String roots
fall back to the existing page transaction. That path reacquires the key lock
and rereads the current root; it does not reuse a stale expired snapshot when
cleaning up a key. This preserves expiry, type errors, legacy import and
concurrent replacement semantics. The fast path emits no read observer events,
matching the previous typed GET page transaction behavior.

RocksDB, WiredTiger, older memory engines and EXEC workspaces keep their
existing read path. Workspaces retain staged read-your-writes and the exclusive
execution clock. No record format, page limit, replication event or writer
protocol changes in this increment. The optimization avoids page transaction
construction for eligible reads; it does not remove the key lock or make TBB
record payloads safe to read without their existing synchronization.

## Validation

`inline_string` checks missing/empty/binary/boundary values, zero-allocation
reads into reserved output, allocation failure, TTL cleanup and WATCH, legacy
import, invalid/unsupported records, large-value fallback and EXEC workspace
publication. A backend probe rejects a fast read that reaches the ordinary
root/page lookup path. Eight reader threads race 1200 writes that switch
between inline and paged values, replication writes and deletion, checking
complete values and exact missing/error distinctions on both memory backends.

## Checked GET measurements

Matched C++11 Release builds use two server threads, four clients, a 64-byte
hot value, 256-command pipelines, 4096 measured batches per worker, 16 warmup
batches and three rotating rounds. Active expiration is disabled. Each backend
run checks 50,331,648 measured responses, including prior and new RESP2/RESP3.
The two backend measurements run sequentially without build/test load.

| Backend / protocol | Prior PKV, requests/s | New, requests/s | Change |
| --- | ---: | ---: | ---: |
| MDB / RESP2 | 1,504,530 | 2,139,908 | +42.23% |
| MDB / RESP3 | 1,534,370 | 2,131,283 | +38.90% |
| oneTBB / RESP2 | 1,487,814 | 2,060,405 | +38.49% |
| oneTBB / RESP3 | 1,455,009 | 2,064,645 | +41.90% |

These macOS ARM64 hot-key results compare PKV revisions. They do not establish
mixed-workload, paged-value, disk-backend or stock Redis performance. Three
samples per variant do not establish statistical significance. The clients and
local machine can constrain throughput. See [MDB raw results](
benchmarks/inline-get-mdb-2026-11-01.json) and [oneTBB raw results](
benchmarks/inline-get-tbb-2026-11-01.json).

This increment adds no commands. Blocking commands, Pub/Sub, scripting, Redis
RDB/AOF, full replication and complete cluster behavior remain active work.

## Full matrix

The frozen source passed 886/886 full-matrix CTests on macOS ARM64: MDB Debug
and C++11 Release 101 each; oneTBB Debug and C++11 Release 110 each; RocksDB
Debug/Release and WiredTiger Debug/C++11 Release 116 each. Debug configurations
used ASan/UBSan. Existing timeouts, concurrency checks and disk synchronization
settings were retained. All 113 recorded source fingerprints remained fixed
throughout the matrix. The measured Release binaries match the full-matrix
binaries, and each backend's old/new GET compile flags match exactly.

The retained protocol checks include 271 typed RESP3 comparisons, 191 stable
HyperLogLog observations, 13 INFO observations and 31 CONFIG observations,
plus the existing ACL, authentication, client, transaction and replication
suites. Modified C++ files satisfy the 80-column limit; new and modified
functions stay below 200 lines. Linux/GCC was not validated in this matrix.
See [machine-readable validation evidence](
benchmarks/inline-validation-2026-11-01.json).
