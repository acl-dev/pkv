# Local expiry-index performance

The retained implementation skips bucket mutexes for proven-empty stripes and
for stripes whose scheduling lower bound is later than the poll time. Hints
live in a separate array, preserving the original mutex/container layout.
Deletes and deadline extensions can leave an earlier hint; due polls correct it
under the bucket mutex. Earlier deadlines publish a new bound only after the
index mutation succeeds. Actual deletion still rechecks stored root metadata.

This is a comparison of two local PKV snapshots. It is not a stock Redis result,
a service throughput comparison or evidence of complete Redis compatibility.
Only `expiry_index.h/.cpp` differ in the measured production code. The frozen
baseline predates this optimization; both snapshots include active expiry.

## Method

`expiry_path_bench` builds with `PKV_BUILD_BENCHMARKS=ON`. The recorded run uses
Apple Clang 17, C++11 Release (`-O3 -DNDEBUG`), macOS ARM64, 10 logical CPUs and
128-byte cache lines. MDB and oneTBB 2021.10.0 share the same binary. There are
no sanitizers in the benchmark. Binary/source hashes and build metadata are in
[the metadata JSON](benchmarks/expiry-path-meta-2026-10-31.json).

Five paired trials alternate before/after order: odd trials run before first,
even trials after first. State order rotates within each trial. No benchmarks,
builds or regression jobs run concurrently. The desktop remains a source of
noise. Samples are short, especially raw SET and empty-index erases; the full
paired ranges below matter. These are observations, not confidence intervals.
No trial or outlier was discarded.

Store cases use 16 physical shards, 256 distinct persistent keys per worker,
128-byte values alternating every 256 operations, and one or four threads.
`disabled` has no expiry index, `empty` builds one with no TTL owners, and
`future` builds one with 256 additional unrelated expiring owners. The deadline
is 4102444800123 ms. Each worker warms up 64 operations before a common start
barrier. The timer includes measured operations and worker joins, excluding
setup/warmup. Final bytes and physical root counts are checked.

The CLI iteration argument is 100000. Each store worker performs that many
writes. Index erase workers perform 100 times that many iterations; poll,
refresh and churn workers perform 10 times that many. Erase probes target
persistent names. Refresh replaces existing future TTLs, toggling the deadline
every 256 operations. One churn iteration is a set/erase pair, counted as one
iteration, not two. Poll uses now=0, limit=64, and runs on one thread. Final TTL
owner counts and empty poll results are checked.

There is no TCP, ACL scheduler, active reaper, replication, disk engine or
concurrent polling in these timed cases. The ordinary `paged_set` path includes
more DAO/transaction work than `raw_set`; the latter writes encoded roots.
Neither is an end-to-end server request benchmark.

## Retained results

Rates are millions of benchmark iterations per second. Before/after columns
are medians of five rates. Ratio is the median of the five paired after/before
ratios, so it need not equal the ratio of the two rate medians. The final column
shows the smallest and largest paired ratio.

| Engine | Operation | Index | Threads | Before M/s | After M/s | Ratio | Range |
| --- | --- | --- | ---: | ---: | ---: | ---: | --- |
| index | churn | empty | 1 | 15.695 | 15.898 | 1.017 | 0.968–1.146 |
| index | churn | empty | 4 | 23.654 | 25.202 | 1.002 | 0.724–1.213 |
| index | erase | empty | 1 | 133.639 | 488.240 | 3.650 | 3.587–3.662 |
| index | erase | empty | 4 | 182.667 | 1588.307 | 8.672 | 8.183–10.105 |
| index | erase | future | 1 | 79.079 | 82.228 | 1.040 | 1.025–1.062 |
| index | erase | future | 4 | 172.725 | 168.888 | 0.978 | 0.688–0.992 |
| index | poll | empty | 1 | 3.635 | 54.788 | 15.053 | 14.096–15.193 |
| index | poll | future | 1 | 3.387 | 31.571 | 9.318 | 9.248–9.362 |
| index | refresh | future | 1 | 21.981 | 21.668 | 0.981 | 0.955–1.141 |
| index | refresh | future | 4 | 19.008 | 18.649 | 0.981 | 0.777–1.027 |
| mdb | paged_set | disabled | 1 | 1.876 | 1.887 | 1.013 | 0.981–1.076 |
| mdb | paged_set | disabled | 4 | 2.908 | 2.965 | 1.005 | 0.898–1.107 |
| mdb | paged_set | empty | 1 | 1.715 | 1.714 | 1.005 | 0.986–1.023 |
| mdb | paged_set | empty | 4 | 2.773 | 2.924 | 0.999 | 0.833–1.183 |
| mdb | paged_set | future | 1 | 1.638 | 1.658 | 1.002 | 0.978–1.044 |
| mdb | paged_set | future | 4 | 2.758 | 2.803 | 0.940 | 0.590–1.240 |
| mdb | raw_set | disabled | 1 | 12.293 | 12.414 | 1.017 | 0.776–1.031 |
| mdb | raw_set | disabled | 4 | 28.456 | 28.748 | 0.980 | 0.920–1.261 |
| mdb | raw_set | empty | 1 | 7.974 | 8.680 | 1.078 | 0.468–1.147 |
| mdb | raw_set | empty | 4 | 20.365 | 23.359 | 1.147 | 1.136–2.037 |
| mdb | raw_set | future | 1 | 7.237 | 7.198 | 0.980 | 0.686–1.059 |
| mdb | raw_set | future | 4 | 18.895 | 19.169 | 1.004 | 0.683–1.415 |
| oneTBB | paged_set | disabled | 1 | 1.843 | 1.906 | 1.018 | 1.016–1.080 |
| oneTBB | paged_set | disabled | 4 | 3.480 | 3.523 | 1.041 | 0.808–1.055 |
| oneTBB | paged_set | empty | 1 | 1.693 | 1.725 | 1.029 | 0.979–1.058 |
| oneTBB | paged_set | empty | 4 | 3.336 | 3.320 | 0.994 | 0.878–1.028 |
| oneTBB | paged_set | future | 1 | 1.657 | 1.672 | 1.008 | 1.002–1.061 |
| oneTBB | paged_set | future | 4 | 3.269 | 3.412 | 1.050 | 0.836–1.104 |
| oneTBB | raw_set | disabled | 1 | 10.565 | 10.417 | 1.005 | 0.954–1.013 |
| oneTBB | raw_set | disabled | 4 | 22.675 | 21.733 | 0.935 | 0.639–1.058 |
| oneTBB | raw_set | empty | 1 | 7.244 | 7.442 | 1.028 | 1.013–1.032 |
| oneTBB | raw_set | empty | 4 | 16.753 | 18.244 | 1.073 | 0.750–1.114 |
| oneTBB | raw_set | future | 1 | 6.363 | 6.314 | 0.995 | 0.988–1.009 |
| oneTBB | raw_set | future | 4 | 16.164 | 15.383 | 0.939 | 0.664–1.031 |

The strongest repeatable effects here are empty polling (15.05x), future
polling (9.32x), and empty-index erase probes (3.65x with one thread and 8.67x
with four). These improve the small scheduling/index operation, not the entire
write command. Default idle reaper ticks are infrequent, so large poll speedups
do not imply a comparable whole-service CPU reduction.

Four-thread refresh is 0.981x and churn is 1.002x in the paired medians. Churn
has a wide 0.724–1.213 range; this does not establish zero TTL-write overhead.
Four-thread nonempty erase probes are 0.978x. MDB future paged SET has a 0.940x
paired median, while oneTBB future paged SET is 1.050x. Disabled controls also
move: four-thread oneTBB raw SET is 0.935x despite having no index. The store
results are mixed and noisy; no general SET throughput improvement is claimed.
Longer isolated service-level runs are needed for latency and workload claims.

## Alternatives and limits

Five earlier alternatives remain in the same raw dataset. Exact minima were
tried at the end/front of each bucket, with acquire/release or relaxed accesses,
and in dense/padded separate arrays. Preserving bucket layout removed a large
nonempty-erase regression, but publishing exact minima on TTL insert/removal
still cost throughput in the churn workload. Padding did not remove that cost
and increased fixed memory. The retained conservative bound avoids most such
publications and uses only 64 extra atomic timestamps: 512 bytes on this host.
These experiments motivated the choice; they are not separate production wins.

| Variant | Four-thread churn paired ratio |
| --- | ---: |
| exact-release-acquire | 0.880 |
| exact-relaxed-tail | 0.886 |
| exact-relaxed-front | 0.892 |
| exact-relaxed-array | 0.850 |
| exact-relaxed-padded | 0.819 |
| lower-bound-array | 1.002 |

The conservative hint may retain an old future deadline after its last owner
is removed. That stripe may continue taking its erase mutex until a due poll
refreshes the hint. It retains no owner nodes and cannot hide an earlier new
TTL. All ordered/hash data access still takes the bucket mutex; the relaxed
atomic publishes only the skip hint. Same-key database locks order mutations.
Allocation failure preserves the earlier hint and marks the index dirty for
repair. There is no new global write counter or persisted-format change.

## Reproduction and validation

[Raw CSV](benchmarks/expiry-path-2026-10-31.csv) includes all 2040 rows from the
six variants, modes and trials. The
[summary JSON](benchmarks/expiry-path-summary-2026-10-31.json) contains all 204
case summaries. Baseline source snapshots are archived beside them as
`expiry-index-before-2026-10-31.h.txt` and `.cpp.txt`. They allow rebuilding the
baseline in a separate isolated checkout of the same surrounding source; do
not overwrite an active checkout or mix builds during comparison. Temporary
binaries are identified by SHA-256, not by a committed repository revision.

With isolated before/after binaries built against the same surrounding source:

```sh
/path/to/before/expiry_path_bench 100000 1 > before-1.csv
/path/to/after/expiry_path_bench 100000 1 > after-1.csv
```

Repeat trials 2 through 5, reversing binary order on even trials. Keep engine,
compiler, flags and benchmark harness identical. The CLI accepts iterations
256 through 1000000 and trial IDs 1 through 1000. Benchmark targets are excluded
from CTest; correctness coverage lives in the active-expiry fixtures.

Validation status is recorded in the
[active-expiry matrix](../tests/README.md#active-expiry-index-optimization).
The tests cover cache movement, timestamp boundaries, allocation rollback,
concurrent readers/writers and cross-thread publication, as well as physical
expiry, reopen, replication and existing command behavior across four engines.
