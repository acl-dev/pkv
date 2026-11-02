# Retained native MOVE storage coordination

`move_storage::run` supplies the physical decision/application phase for a future
online MOVE adapter. It holds exclusive execution admission and both logical
key locks continuously across preparation, the source WAL fence, durable
publication, bounded application, completion effects and journal removal.
The same lock frame and physical application code now serve
[startup recovery](MOVE_STARTUP.md).

This is an internal component. It is not connected to the command dispatcher,
server journal owner or replication receiver. Public MOVE, the logical policy
and effects adapter, grouped cross-DB replication and constant-time volatile
page-family transfer remain unfinished. Supplying empty effects in a live
server would violate this component's contract. Isolated tests can exercise
physical application without pretending that it is a complete Redis command.

## Adapter contract

The caller binds a journal to the exact DB pool configuration and finishes
startup recovery before using it. Other access to that journal must serialize
through the same exclusive execution admission. Call without an existing shared
admission, as required for EXEC; do not attempt to upgrade a shared admission.

The coordinator takes an owning key snapshot before acquiring locks. That same
snapshot addresses the locks, bounded reads, WAL fences, journal and native
application. The callback view exposes this immutable owner through completion;
changing a caller's original string cannot redirect storage operations.

Both `prepare` and `complete` callbacks are mandatory. Under both key locks,
`prepare` receives immutable bounded source/target root snapshots and presence
flags. It must validate logical type, expiry and target absence, codec
compatibility and any applicable command restrictions. It must preallocate
logical effects and publication state before returning `proceed`. `skip` makes
no decision; `error` or a predecision exception returns a storage error.
Proceeding with a physically missing source is rejected by the coordinator.
A raw target root can be replaced only when the adapter has established that
it is logically expired. The physical layer does not decode or rewrite roots.

`complete` runs after both physical sides are durable and before journal
removal. It must publish both DBs' WATCH, expiry and readiness effects and an
appropriately grouped observer/replication event under the retained locks.
It cannot implement these effects through recursive public DB operations.
No callback may reenter either DB or mutate the journal. Diagnostic observers
may inspect journal state and must obey the same ownership rules.

No adapter implementing that full policy/effects/publication contract is added
in this revision. The native fixture uses metadata-only expiry/absence policy
and an isolated completion probe. That fixture policy is not a Redis handler.

## Storage ordering and failures

1. Acquire exclusive admission, validate distinct DB IDs/objects and complete
   native initialization, then lock source/target key stripes in DB address
   order. Unsupported native capabilities decline before callbacks or writes.
2. Require an empty journal and a next nonzero serial. Load roots through
   bounded native lookups, then run preparation. A serial overflow, invalid
   ticket or missing required callback is rejected before publication.
3. Synchronize the source WAL while retaining those locks. On failure, return
   an error with no decision or value mutation.
4. Activate fail-stop protection before attempting `decide`. From this point,
   publication failure or an exception aborts before either key lock is
   released; an unacknowledged rename may already be durable.
5. Apply through the same bounded physical phase as startup redo. Clear the
   target family, stream at most one 16-KiB page per commit, publish its root
   last, synchronize the target, erase the source and synchronize it again.
   Reuse the retained predecision source fence instead of performing it twice.
6. Invoke completion effects, clear the journal with directory synchronization,
   then return success and release the locks. Exceptions even in the final
   diagnostic callback abort before unlocking.

Before decision publication, failures can return normally because no physical
MOVE change has occurred. Once publication is attempted, no partial result is
returned to a caller. The guard is constructed after both key locks and before
any such attempt; its destructor runs before lock destructors during unwinding.
Startup also activates this guard before root loading can allocate or fail.

A failed journal removal directory sync is different from an unlink failure:
the running filesystem can already show no decision file while both physical
DBs are complete. Reopening must accept either a surviving valid decision or
an absent one, according to the existing [journal contract](MOVE_RECOVERY.md).
This process-interruption observation does not prove power-loss behavior.

`last_serial` and `can_decide` are nonmutating journal inspection methods.
Validation does not replace or synchronize a record. Existing `decide` and
`complete` state/error semantics and the persisted format remain unchanged.
Serials advance within one locked journal lifetime; they are not replicated
transaction IDs and need not persist after an empty journal is closed.

## Bounds and remaining integration

Root snapshots stay at most 16 KiB each. Private page application stays one
bounded page per native commit. Binary/empty keys, native metadata and absolute
expiry bytes are preserved; no collection is serialized into a single value.
The decision carries the owner key, including the journal's existing 512-MiB key
bound, rather than collection payloads. Engine caches, decompression and
allocator RSS are outside the PKV staging bound. Root loading preflights the
physical root bound, not the complete type-specific page graph. Corruption or
IO failure discovered while streaming after a decision therefore aborts and
requires recovery or storage repair; no valid-media guarantee is introduced.

No per-key fields, ordinary write policy, codec implementation or protocol
command registration are changed. Native memory engines continue using their
existing prepared commit primitives. This disk coordinator explicitly declines
volatile stores; their MOVE adapter still needs an ownership-transfer path.
No throughput improvement, new Redis superiority comparison, TSan run or
standard Redis replication support is claimed.

The next integration must provide the actual policy and paired logical effects,
retain server control ownership through online use and shutdown, and publish
cross-DB replication atomically. Calling the startup entry point after dropping
source locks would lose the validation/WAL/decision invariant and is not an
online MOVE implementation.

## Validation

`move_storage` checks preparation outcomes, source fence failure, unsupported
stores, invalid IDs, serial overflow, missing completion, retained exclusion
against two real writer threads, and completion ordering. A callback mutating
the caller's key string must still transfer the original owner. An independent
ASan probe reproduced that bug before the owned-key fix; the same probe source
is retained for before/after execution against the two core revisions.

`move_storage_rdb/wdb` uses real native stores, two physical roots per DB, an
8-MiB String, binary/empty keys and Hash/List/Set/ZSet values with 48-KiB blobs
or members. CRC, root bytes, page count, dense indexes and subsequent mutations
check the result. Native hook counts require two source WAL fences and one
target fence, with the first source fence preceding decision publication.

Eight process-interruption points cover validation, source fencing, durable
decision, page application, target publication, source removal, effects and
completion. Sixteen failure paths cover predecision reads/preparation, journal
publication, native page/root/fence/erase errors, journal cleanup and callback
exceptions. Every interrupted or failed case reopens and checks twice.

The first RocksDB fixture incorrectly required a surviving file after a failed
unlink directory sync. Its preserved failure led to a stricter distinction:
that running filesystem must show the completed data even if the log is absent;
publication failures without a decision must retain the original data. This
was a test correction, not a change to journal persistence behavior.

The exact source states, native logs and stable-key probe are recorded in
[coordinator validation](benchmarks/coordinator-validation-2026-11-01.json).

The final revision passed **374 selected CTests** across eight C++11 Release
and C++17 ASan/UBSan configurations: MDB 38/38, oneTBB 46/46, RocksDB 51/51
and WiredTiger 52/52. This is a targeted selection, not the full CTest suite.
All 614 frozen code/test/build files match the tested state; 600 parent files
remain byte-identical. The standalone stable-key probe uses identical source
and flags: the retained pre-fix executable exits 1 on the wrong-owner result,
while the corrected executable exits 0. Existing journal implementation bodies
reconstruct byte-for-byte to the parent after removing additive inspections.


The complementary [memory ownership component](MEMORY_MOVE_FAMILIES.md) now
prepares both volatile engines and swaps their native page tables without a
page-copy vector. It already publishes local WATCH/expiry/readiness effects;
its completion callback supplies observer/replication publication. The disk
component's completion contract still requires those local effects as well.
Neither component is connected to a public MOVE command yet.
