# Prepared page commits and cross-database coordination

This is storage groundwork for atomic cross-database mutations. It does not
register MOVE or claim complete cross-database Redis command support.

MDB and oneTBB now provide a preparation callback while retaining their native
locks and rollback state on the stack. Preparation can allocate or fail. An
unpublished frame removes prepared additions and restores replaced page families
before releasing its locks. Publication only swaps prepared bytes and deletes
entries; it is declared `noexcept` and performs no C++ heap allocation.
Ordinary single-database commits reuse the same stack state without allocating
a separate transaction handle or acquiring a global write mutex.

`db::transact_pages_with` provides two bounded page views. The caller must enter
without shared execution admission, as with EXEC. The primitive takes exclusive
admission and locks DB/key identities in a total order. The callback stages all
changes before either engine prepares them. Validation and observer capacity
checks run before preparation; then both engines prepare in DB-address order.
Only after both preparations succeed does the coordinator publish both frames.
No compensating write/delete is used. The same-database case delegates to one
ordinary atomic page transaction, with identical source/target rejected.

Failure of the second preparation unwinds both native frames. Logical effects,
WATCH invalidation, generic wake hints and external page observers run after
both publications. Effects for both databases precede either observer callback.
External observer allocation failure retains the existing committed-write
semantics; it cannot report the native commit as rolled back. Expiry tracking
retains its existing dirty-index fallback on allocation failure.

Native root/page sizes remain bounded by `page_transaction::max_value_bytes`.
The coordinator uses the same staged page vectors as existing transactions;
it does not serialize a whole List/Hash/Set/ZSet into one root value. Staging
memory and lock duration can still grow with the number of touched pages.

## Remaining prerequisites for MOVE

The internal preparation capability currently belongs to MDB and oneTBB.
Independent RocksDB instances have independent WALs; independent WiredTiger
instances likewise cannot join this in-memory publication decision. Those
engines decline cross-database coordination before invoking the callback or
writing data. Staged actor-local workspaces also decline cross-database use.
Same-database native transactions retain their existing backend support.

A durable coordinator/recovery protocol or a shared transactional storage domain
is still required for disk cross-database writes. Simply committing a copy and
then deleting its source cannot guarantee rollback if the second write fails.
The custom replication protocol also emits separate DB-scoped observations;
this primitive does not add atomic cross-database replay or durable observer
acknowledgment. MOVE must account for those limitations before public command
support is claimed. A public memory MOVE also needs an ownership-transfer fast
path where native representations are compatible, rather than staging every
payload page. No disk format, command catalog or existing on-disk data
is changed by this groundwork.

## Verification

`page_pair` exercises MDB/oneTBB pairs, including mixed memory engines. It
transfers binary-named roots and 16-KiB private pages, preserves absolute expiry,
replaces destination page families, and checks root counts and deterministic
rank selection after success and rollback. Allocation sweeps cover both empty
and populated destinations. Explicit barriers fail the second engine before
and after its preparation; a separate MDB case rejects all C++ allocations
after both preparations and still publishes successfully.

Additional cases cover callback rejection, poisoned page addresses, native
read failure, unsupported engines, observer preflight rejection, no-op writes,
same-database delegation, WATCH and wake boundaries, and opposite-direction
concurrent transfers with coherent root-count snapshots. Observer probes check
that both native writes and WATCH effects precede either external callback.

The final matrix passed **1,094 CTests** across eight MDB, oneTBB, RocksDB and
WiredTiger configurations, including C++11 and ASan/UBSan builds. Five focused
ASan/UBSan fixtures each passed five additional repetitions. All 573 frozen
source fingerprints, binary/runtime hashes and allocation-budget results are
recorded in the [validation report](
benchmarks/pair-validation-2026-11-01.json). The sanitizer builds use the already
identified private ACL fiber archive with stack registration; Release builds
use the original installed runtime. No TSan run is claimed.

The full matrix covers 4,480 paired allocation budgets. Each engine pair covers
320 budgets, split between empty and populated destinations. oneTBB builds
exercise all four memory-engine combinations; other builds exercise MDB pairs.
Disk-enabled builds also check their real engine capability rejection.
Compiled disk engines are tested without opening storage to verify that the
capability rejection precedes data lookup. This does not verify a disk
cross-database transaction or crash recovery protocol.

Matched C++11 Release measurements use retained dense-index parent binaries
with identical core flags. GET comparisons verify 100,663,296 replies in total;
write comparisons verify 50,331,648 timed operations. Median changes were:

| Backend | GET RESP2 | GET RESP3 | Overwrite | Insert/delete | Retained fill |
| --- | ---: | ---: | ---: | ---: | ---: |
| mdb | +0.50% | -1.28% | -1.25% | -3.15% | -1.89% |
| tbb | -0.96% | +0.47% | +2.32% | +0.80% | +1.32% |

Raw samples and process RSS are available for
[MDB GET](benchmarks/pair-get-mdb-2026-11-01.json),
[oneTBB GET](benchmarks/pair-get-tbb-2026-11-01.json),
[MDB writes](benchmarks/pair-updates-mdb-2026-11-01.json) and
[oneTBB writes](benchmarks/pair-updates-tbb-2026-11-01.json).
These are ordinary single-database workloads checking the refactor's effect,
not cross-database command throughput. Three rotating samples do not establish
statistical significance, general performance improvements or superiority over
official Redis. The locally modified Redis 8.6.2 reference is used for semantic
checks only. The earlier unexplained RocksDB Release startup SIGSEGV remains
an unresolved historical limitation, not a claimed fix.

The [fresh command inventory](
benchmarks/pair-command-inventory-2026-11-01.json) remains 230 PKV names versus
412 local-reference names, with 183 missing names and cluster|addslave as an
extra alias. MOVE remains missing, pending the prerequisites above. Full Redis
compatibility and performance remain the active goal.

The initial MDB insert/delete median fell 3.15%. A predeclared separate
[eight-round follow-up](benchmarks/pair-churn-followup-mdb-2026-11-01.json)
checked another 33,554,432 operations and measured a 2.36% lower median.
Both raw sample sets remain intact. Ordinary MDB commit overhead remains an
optimization requirement; this groundwork is not a throughput improvement.


A subsequent [native-buffer optimization](NATIVE_COMMIT_BUFFERS.md) removes
unused MDB scratch allocations while preserving this preparation boundary.
Its three-variant screen compares both retained parents; the original slowdown
samples above remain historical evidence. MOVE prerequisites remain unchanged.


The subsequent [WAL barrier primitive](DURABILITY_BARRIERS.md) synchronizes an
initialized disk domain under its logical key stripe and rejects partial-open
instances. It supplies a source-durability prerequisite, while durable decision
logging/recovery, atomic cross-DB replay and public MOVE remain unfinished.


The subsequent [memory family handoff](MEMORY_MOVE_FAMILIES.md) reuses these
prepared frames for an ownership transfer. A separate cold handle prepares
without moving data, then swaps whole page maps before both native roots
publish. Existing ordinary commit bodies and frame fields remain unchanged.
Local effects are paired; complete observer/replication and command adapters
remain required before public MOVE can use this path.
