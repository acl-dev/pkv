# Allocate native commit buffers on demand

MDB now reserves each temporary commit buffer only when preparation first needs
it. Deletion does not reserve replacements, new roots or page resets. An
existing-root overwrite does not reserve additions that it will never publish.
This removes unnecessary C++ heap allocations while retaining the same native
locks, preparation/publication order and rollback state.

Each buffer still reserves the existing upper bound on first use, so a large
batch does not grow its bookkeeping one element at a time. Reset bookkeeping
is gathered before any old page map is swapped. New page-owner bookkeeping is
reserved before inserting that owner; a later exception therefore cannot leave
an inserted owner absent from rollback tracking. Root and page additions are
still tracked before insertion, and publication still performs no allocation.

Only `db/mdb/mdb_batch.cpp` changes in production. Codecs, paged value layouts,
DAO/actions, oneTBB, disk backends, readiness helpers and paired coordination
remain unchanged. String/List/Hash/Set/ZSet payloads retain bounded native
pages. The memory fast path does not acquire a new global write lock.

## Allocation contract

A private engine probe measures only native submission, with one existing
128-byte root and no page deltas. It excludes command parsing, logical key
locking, codec work, notification preparation and network IO:

| Native operation | Paired-parent allocations | Candidate allocations |
| --- | ---: | ---: |
| Existing root overwrite | 6 | 4 |
| Existing root erase | 5 | 2 |

The before/after executables use the same probe source, compiler flags and
installed ACL runtime. The project fixture `native_commit_buffers` uses
thread-local counters, verifies these allocation bounds, and reads back root
bytes/counts after commit. These counts describe the tested standard library
and native operation, not all allocations performed by a Redis command.

The existing `page_pair`, `page_storage`, `dense_index` and workspace fixtures
exercise callback aborts, allocation failures, page-family restoration,
root/index bijections and publication/notification boundaries. Deferred
reservation must retain those guarantees; fewer allocations do not justify
weakening rollback or publishing either cross-DB frame early.

## Performance and remaining work

A predeclared six-round three-variant screen compares retained dense-index
(pre-refactor), paired-preparation and candidate C++11 Release binaries. Orders
rotate and reverse to balance positions. Every reply and final root count is
checked. The same benchmark helper, core flags and runtime are used. These
local short samples do not establish statistical significance or throughput
against official Redis. Initial parent slowdown reports remain intact.

The [prepared commit foundation](PREPARED_PAGE_COMMITS.md) still has the same
scope. This optimization does not add MOVE, durable coordination between disk
engines, grouped cross-DB replication or a native ownership-transfer fast path.
Full Redis compatibility and performance remain the active goal.

The final matrix passed **1,102 CTests** across eight MDB, oneTBB, RocksDB and
WiredTiger configurations, including C++11 and ASan/UBSan. Six focused
ASan/UBSan fixtures each passed five additional repetitions. The full matrix
retains 4,480 paired allocation budgets, plus the existing sampling/dense
fault sweeps and their root/page/WATCH/notification checks. All 574 source
fingerprints and 571 unchanged parent sources are recorded in the
[validation report](benchmarks/lazy-validation-2026-11-01.json).
Sanitizer builds retain the identified private ACL fiber archive with stack
registration; Release comparisons use the original installed runtime. No TSan
run or broader-platform allocation-count claim is made.

The [three-variant screen](benchmarks/lazy-churn-screen-mdb-2026-11-01.json)
checked 37,748,736 timed operations across 18 samples. Candidate MDB churn
median was +4.03% against the paired-preparation parent and +2.12% against the
retained dense-index revision. The parent
[eight-round slowdown measurement](benchmarks/pair-churn-followup-mdb-2026-11-01.json)
remains available; no original samples were overwritten. This screen describes
a local workload, not proof of an overall or official Redis throughput gain.

The final separate matched C++11 Release comparisons use the paired-preparation
parent and unchanged core flags. GET verifies 100,663,296 replies in total;
write comparisons verify 50,331,648 timed operations. Median changes were:

| Backend | GET RESP2 | GET RESP3 | Overwrite | Insert/delete | Retained fill |
| --- | ---: | ---: | ---: | ---: | ---: |
| mdb | +0.42% | -0.10% | +6.30% | +4.62% | +4.36% |
| tbb | +1.37% | -0.67% | -1.09% | -2.07% | -0.08% |

Raw samples and process RSS appear in
[MDB GET](benchmarks/lazy-get-mdb-2026-11-01.json),
[oneTBB GET](benchmarks/lazy-get-tbb-2026-11-01.json),
[MDB writes](benchmarks/lazy-updates-mdb-2026-11-01.json) and
[oneTBB writes](benchmarks/lazy-updates-tbb-2026-11-01.json).
Small short-run differences do not establish statistical significance. oneTBB
code is unchanged; its measurements serve as a control. Removing temporary
buffers does not remove dense-index per-key metadata or retained capacity.
RSS includes runtime, allocator and caches, not exact index/buffer bytes.

The [fresh name inventory](benchmarks/lazy-command-inventory-2026-11-01.json)
remains 230 PKV versus 412 locally modified Redis 8.6.2 reference names, with
183 missing names and cluster|addslave as an extra alias. Name presence does not
prove semantic parity. Full Redis, Streams, scripting, standard replication,
durable cross-DB coordination and public MOVE remain unfinished. The prior
unexplained RocksDB Release startup SIGSEGV remains an unresolved historical
limitation; passing this matrix does not establish its cause or a fix.
