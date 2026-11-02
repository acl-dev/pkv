# Physical WAL durability barriers

`db::sync_key(owner)` requests a synchronous WAL fence for the physical storage
instance selected by an owner's key. It also works for an empty, deleted or
missing owner: durability of an erase cannot depend on the key still existing.
The call does not look up/decode a record, materialize pages, change expiry,
invalidate WATCH or emit logical write/readiness notifications.

The wrapper retains ordinary shared execution admission and the owner's
existing logical key stripe throughout the engine call. A same-stripe writer
cannot cross that fence; the wrapper adds no node-wide exclusive admission.
Engines can still synchronize their own WAL internally. Once the call returns
and releases the key lock, later writes are not covered. Transaction callbacks
must not recursively call this public wrapper while already owning key locks.
A future cross-DB coordinator must call the engine hook under its retained
locks from source validation through decision publication.

## Engine semantics

RocksDB calls `FlushWAL(true)` on the same CRC-selected physical instance used
for that owner's roots, pages and scan index. Its installed API specifies that
the call also synchronizes the WAL; it covers all completed writes in that
instance, rather than persisting only one key. Success returns `ok`, an engine
unsupported result remains `unsupported`, and other failures return `error`.
An engine that has never completed a successful open returns `error` before
selecting a shard, including a partial multi-store initialization failure.

WiredTiger borrows an idle native session and calls `log_flush("sync=on")` on
the selected connection. Native tables and the connection already enable
logging; ordinary native commits continue to use `commit_transaction` with
`sync=on`. An active transaction or unopened session is rejected rather than
silently rolling back work to perform the fence. Session acquisition failure
and engine flush failures return `error`.

MDB, oneTBB and staged actor-local workspaces return `unsupported`. They do not
claim that volatile memory survives a process crash. Ordinary GET/SET/page
commit paths do not call this API, and RocksDB's existing ordinary write options
are unchanged. No public Redis command or on-disk layout is added.

## Verification

The `durability` fixture checks status propagation, allocation exceptions,
volatile/workspace rejection and unopened/failed-open disk engines. It retains
value bytes, absolute expiry, WATCH validity, observer counts and generic wait
state across
success/failure. A blocked fake engine fence prevents same-key writes while a
writer on an independent stripe completes, checking the wrapper's admission
scope independently of storage IO.

Disk fixtures exercise binary/empty keys, private 16-KiB pages, single/multiple
physical stores, partial initialization failures, cached/reopened sessions,
root/scan counts and deletion followed by a fence on the now-missing owner.
The crash fixture runs six writers whose
acknowledgments follow successful fences, and two acknowledged deletions; it
kills only its own child and checks roots/pages/indexes after independent
reopen.
A later in-flight epoch may recover, but the last acknowledged epoch must not
be lost and page sets must not mix epochs.

These are process-kill recovery tests, not simulated hardware power failures.
The API depends on the storage engine, filesystem and device honoring their
synchronous WAL contract. The unit error probe verifies the PKV boundary; it
does not inject every real device/engine IO failure. Such fault coverage remains
necessary for the durable cross-engine coordinator.

## MOVE prerequisites

This primitive is groundwork, not a cross-database transaction. A command-only
MOVE decision log must first make the locked source snapshot durable. Otherwise
a durable decision could outlive the async source write it intends to move.
Target persistence must precede source removal, and both must be durable before
clearing the decision. Errors after a durable decision cannot be reported as a
rolled-back command while admitting traffic into a half-applied state.

A durable decision/recovery protocol, grouped cross-DB replication/replay and
compatible native owner-family transfer remain unfinished. Public MOVE is still
missing. Full Redis compatibility, standard replication, Streams, scripting and
broader administration remain the active goal. See the
[prepared memory boundary](PREPARED_PAGE_COMMITS.md) for the existing volatile
coordination path and its limits.

The final matrix passed **1,118 CTests** across eight MDB, oneTBB, RocksDB and
WiredTiger configurations, including C++11 and ASan/UBSan. Seven focused ASan
fixtures passed five further repetitions (35 passes). Both native disk barrier
fixtures passed three further repetitions on each ASan disk build (12 passes),
for **47 additional passing CTests**. Each crash fixture contains six
acknowledged write epochs and two acknowledged deletions. The full matrix also
retains the existing 4,480 paired allocation budgets and native 4/2 allocation
bounds. All 581 frozen source hashes, 566 unchanged parent sources, dependency
header fingerprints, runtime/binary hashes and logs appear in the
[validation report](benchmarks/sync-validation-2026-11-01.json).

Initialization testing found a boundary in the first candidate: with a valid
first storage path and a later path that is a regular file, both engines could
return `open=false` but a successful fence on the partial instance. Retained
before/after native probes show that the final wrapper returns `error` before
calling the engine unless a complete open has succeeded. A failed reopen attempt
on an already initialized instance does not revoke its previous initialization.
The old matrix was deliberately stopped for this correction and its results are
excluded from the final matrix. Real partial-open and fake-engine gate cases
are now regression tests.

The barrier revision left ordinary hot methods otherwise unchanged.
Removing the added fence method from the WT wrapper and the initialization
assignment from DB open reconstructs their exact parent sources. This is
recorded with the reconstructed DB header in the validation artifact. The
private initialization flag is per database instance, not per stored key.

Separate matched C++11 Release comparisons use retained lazy-buffer parent
binaries and identical core flags. GET checks 100,663,296 replies in total;
ordinary write workloads check 50,331,648 timed operations. Median changes were:

| Backend | GET RESP2 | GET RESP3 | Overwrite | Insert/delete | Retained fill |
| --- | ---: | ---: | ---: | ---: | ---: |
| mdb | -1.09% | +1.61% | +0.52% | +0.79% | -1.16% |
| tbb | -0.51% | +0.60% | -2.18% | -0.55% | -2.03% |

Raw samples and RSS measurements are available for
[MDB GET](benchmarks/sync-get-mdb-2026-11-01.json),
[oneTBB GET](benchmarks/sync-get-tbb-2026-11-01.json),
[MDB writes](benchmarks/sync-updates-mdb-2026-11-01.json) and
[oneTBB writes](benchmarks/sync-updates-tbb-2026-11-01.json).
These ordinary workloads do not invoke the WAL barrier. They check the cold API
change's effect on existing paths, not fsync throughput or cross-DB commands.
Three rotating samples do not establish statistical significance, broad gains
or throughput against official Redis. ASan builds retain the private ACL fiber
archive with stack registration; Release builds use the original installed
runtime. No TSan run is claimed.

The [fresh command inventory](benchmarks/sync-command-inventory-2026-11-01.json)
remains 230 PKV names versus 412 locally modified Redis 8.6.2 reference names,
with 183 missing names and cluster|addslave as an extra alias. Public MOVE and
standard replication remain missing; name presence does not establish parity.
The earlier unexplained RocksDB Release startup SIGSEGV remains an unresolved
historical limitation, not a claimed fix from this passing matrix.


The subsequent [MOVE decision journal](MOVE_RECOVERY.md) persists the command
identity with checked file/rename/directory ordering. It remains separate from
source/target engine application; the barrier and journal components alone
do not implement atomic MOVE or cross-DB replay.


The later [native MOVE redo](NATIVE_MOVE_REDO.md) invokes these raw WAL hooks
under retained key locks, synchronizes the target before source erase and
synchronizes the source again before journal removal. It also corrects WT
positioned-read error classification. Optional [bound startup recovery](
MOVE_STARTUP.md) now validates a persistent domain and runs redo before clients,
expiry or RPC activity. A [retained physical coordinator](MOVE_STORAGE.md)
now supplies the locked validation/fence/decision/application phase. Online
policy, logical effects and grouped replication remain outstanding.
