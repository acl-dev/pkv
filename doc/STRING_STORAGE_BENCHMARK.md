# Native String storage microbenchmark

Measured locally on 2026-10-30 using `tests/string_storage_bench.cpp`.
The [raw CSV](benchmarks/string-storage-2026-10-30.csv) contains all 72 samples.
This compares pkv's legacy JSON String DAO with its native paged String DAO,
using the same Release executable and the same storage engine.

## Method

- AppleClang 17, C++11, `-O3 -DNDEBUG`, sanitizer disabled.
- oneTBB 2021.10.0, yyjson 0.7, installed ACL libraries.
- One calling thread; fresh 16-bucket mdb or oneTBB database for each sample.
- Values contain repeated `x` bytes; initial sizes are 1 KiB, 1 MiB and 8 MiB.
- Read a two-byte middle range or append one byte; verify each operation and
  the complete final value. Setup/final verification are outside the timer.
- Three trials, alternating DAO order. Reads have five warmup operations.
  Reads use 1000 iterations for 1 KiB and 100 for larger values; appends use
  30 iterations. The table is the median of the three per-sample means.
- The final run started after this task's builds and CTests had finished, and
  completed before the next CTest run. Other machine activity was uncontrolled.

## Results

All latency columns are microseconds per DAO operation; lower is faster.

| Engine | Initial size | JSON range | Native range | JSON append | Native append |
| --- | --- | --- | --- | --- | --- |
| mdb | 1 KiB | 0.432 | 0.316 | 1.074 | 0.938 |
| mdb | 1 MiB | 210.917 | 0.611 | 443.760 | 0.818 |
| mdb | 8 MiB | 1702.110 | 0.508 | 3737.146 | 0.810 |
| oneTBB | 1 KiB | 0.430 | 0.286 | 0.993 | 0.894 |
| oneTBB | 1 MiB | 201.202 | 0.491 | 444.078 | 0.726 |
| oneTBB | 8 MiB | 1699.897 | 0.514 | 3713.668 | 0.704 |

Large-value operations avoid parsing or serializing the complete String. The
native DAO reads only the requested chunks and stages only changed chunks plus
the root. Small values also benefit from direct PSR opening and the single-key
transaction lock fast path. The 1-KiB append sample crosses the inline threshold;
that first transition is included in its average.

These are warm-memory, single-thread DAO measurements. They do not measure TCP,
fiber scheduling, multi-core scaling, disk durability, replication, GET of a
whole value, large SET, mixed workloads, tail latency or memory consumption.
They establish no throughput advantage over Redis. Fixed payloads, short loops,
clock precision and machine noise limit generalization, especially below one
microsecond. The allocation/locality and concurrent correctness fixtures provide
separate evidence; benchmark timings are not test assertions.

The `before-fast-open` and `before-single-key-lock` CSV files are exploratory
runs from intermediate revisions under uncontrolled load. Use the final CSV
above for the current implementation, not those earlier runs.

## Reproduce

Configure the normal server build with `PKV_BUILD_BENCHMARKS=ON` and optionally
`HAS_TBB=ON`, then build and run `string_storage_bench`. It prints CSV to stdout
and exits nonzero on an operation or final-value mismatch. A build without
oneTBB emits 36 samples instead of 72. Compare matching build settings and
libraries; sanitizer timings should not be compared with these Release results.
