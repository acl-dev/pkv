# Atomic replay of memory database groups

A complete private v3 EXEC envelope can now replay across multiple MDB and
oneTBB databases through `exec_batch::apply(database_pool&)`. The live
`cluster_client` receiver uses this path after the outer commit frame arrives.
Single-database groups retain the existing `apply(db&)` implementation.
Existing primary cross-DB EXEC already collects its per-command changes into
a v3 envelope under exclusive admission. The new receiver can apply that
envelope to memory DBs. This does not add a primary multi-DB workspace or
change per-command error handling.
This closes the receiver-side atomicity gap needed by future MOVE publication;
it does not register a public MOVE command or change primary EXEC policy.

## Preparation

The receiver owns exclusive execution admission throughout validation,
preparation, publication and local effects. Like EXEC, the caller must enter
without an existing shared admission. All database IDs are checked against the
pool before opening any missing database. Each selected instance must be open
and explicitly support owned native preparation. Different IDs aliasing one
instance are rejected. MDB/oneTBB mixtures are supported. Workspace and disk
instances decline before any native frame is prepared; there is no sequential
write/compensating-delete fallback.

The coordinator acquires all affected logical key mutexes in pointer order,
deduplicating shared stripes. It retains these guards until native rollback or
successful logical publication finishes. Each DB gets a private page workspace.
Original per-DB batch order is preserved, including erase/recreate and
maintenance history. Every prefix is validated against that workspace: a later
root creation cannot make an earlier page write to a missing owner valid.
Staging never changes the native source DB.

Each workspace flattens only its touched roots/pages. It does not clone an
existing native page family. WATCH history, absolute deadlines and final
List/ZSet readiness are prepared before any native frame retains its locks.
Readiness ordering uses the original received updates for each DB.

## Native publication

A new cold native hook returns an owning `prepared_batch` handle. MDB/oneTBB
allocate their existing commit state, gather locks and prepare touched entries.
The ordinary stack-based commit paths, commit-state fields and existing gather,
prepare, publish and rollback implementations remain unchanged. Destruction of
an unpublished handle rolls its DB back while its borrowed root/page inputs
are still alive.

Native frames are prepared in DB-address order using a loop. Database count
does not introduce a recursive callback stack. Any preparation failure destroys
all prepared frames before the outer logical key guards are released. Only
when every frame exists does the coordinator publish all frames through their
nonthrowing native methods. Local WATCH, expiry and generic/List/ZSet readiness
effects follow for every DB. Replication replay emits no observer events, so it
does not echo the received mutations back into the source stream.

Once publication begins, an unexpected exception aborts before any key guard
can unlock. The caller cannot receive an ordinary rollback error after a
partially published group. Native publications require no new allocations;
expiry-index allocation handling retains its existing invalidation/rebuild
behavior.

## Protocol and scope

The wire format and negotiation stay private PKV v3. No group is applied on a
prefix or before negotiation. Cluster mode rejects cross-DB groups. A receiver
pool must share the receiver's actual DB 0 instance. Invalid/missing/unsupported
DBs and root read errors cannot cause a native committed prefix. A failed pool
factory can still have ordinary opening side effects; those are not data
publication.

Existing envelope limits remain 64 MiB and 65536 records, with each root/page
bounded to 16 KiB. Keys retain the private frame limit of 65525 bytes.
This receiver consumes explicit page deltas; it does not
infer an ownership move from a source erase and a target metadata write.
Future source-side memory MOVE publication must provide a complete supported
group, or introduce a separately negotiated bounded transfer protocol for
families beyond those envelope limits. Root-only target events remain invalid
for nonempty transferred families.

Cross-disk replay needs a durable group decision/recovery protocol. Native
MOVE journals do not yet implement arbitrary received EXEC groups. This work
provides volatile memory atomicity, not restart durability or standard Redis
PSYNC replication. Public MOVE, source-side MOVE publication, Streams,
scripting and full Redis coverage remain unfinished. No server throughput or
official Redis comparison is claimed.

## Verification

The `replica_group` fixture exercises three DB IDs (0/7/15), both memory engines,
512 allocation budgets per engine, failure at every native preparation ordinal,
page data/address rollback, WATCH preservation/invalidation, maintenance history,
concurrent writer exclusion and zero-allocation native publication. Other cases
cover 64 DBs with empty keys, 300 owners sharing lock stripes, missing-owner
prefix rejection, invalid/unopened/aliased/unsupported DBs and unchanged
single-DB replay.

A real TCP primary/replica test also publishes changes across DB 0/1, checks
outer-commit buffering and rejects an invalid later DB without changing either
valid DB. The previous expectation that every cross-DB group must disconnect
was updated. Disk configurations retain rejection coverage and also start MDB
for success/coherence checks. Both intermediate test expectation failures and
the corrected runs are retained separately.

Wire tests send/read real v3 frames through local socket pairs and apply them
through the live receiver, verifying unchanged native images until the outer
commit. Typed cases replay Hash/List/Set/ZSet with 48-KiB blobs or members and
then mutate the destination. List readiness and indexed active expiry are
checked independently. Allocation probes count C++ global `new`; they do not
claim all malloc/RSS coverage, TSan evidence or storage power-loss durability.

Eight configurations completed all-target builds and 356 selected checks:
MDB Debug/Release 37 each, oneTBB Debug/Release 44 each, RocksDB Debug/Release
48 each, and WiredTiger Debug/Release 49 each. This is a selected regression
matrix, not the full CTest suite. Four Debug builds use ASan/UBSan; their
registered fiber runtime and all Release/C++11 flags are fingerprinted.
The final source manifest has 628 entries: 612 unchanged from the memory-family
baseline, 10 modified and six added. After the initial focused runs, only the
TCP integration test changed; all production and C++ unit sources stayed fixed.

The source/build/runtime/log fingerprints and exact selected tests are recorded
in [scoped validation](benchmarks/group-validation-2026-11-01.json).


[Replica read retention](REPLICA_READ_RETENTION.md) prevents expired client reads from
erasing pages needed by delayed primary deltas. Logical reads still report
missing data; authoritative replay and explicit writes keep their native paths.

[Atomic memory page-family plans](ATOMIC_FAMILY_PLANS.md) now provide a cold
native publication primitive for final family permutations and page patches.
The ownership-aware transaction view, source publication and negotiated wire
integration still need to use it; existing v3 replay remains unchanged.
