# Prepared MOVE observation and source capture

The source observation API now captures bounded immutable family intents before
native publication. It is available to an online MOVE adapter; the public MOVE
command and its policy/ACL/journal integration remain pending.

## Ownership contract

Call `db::prepare_move_observers` inside the native coordinator's prepare
callback, using its exact source/target raw roots and target-presence flag. The
caller retains exclusive execution admission, and the coordinator retains both
key locks. This API only inspects observer metadata; it neither reads nor writes
DB records. Observers must remain alive until the returned object is destroyed.
Observer registration remains a startup operation.

Each observer must own every input needed after its prepare call; retaining a
borrowed reference to the caller's `family_move` or root strings is unsafe.
Cancellation concerns observation resources and queue reservations. It does not
undo a physical transfer that the caller has already committed.

Preparation returns an owned `family_publication`, or an empty successful result
when there is nothing to publish. On success, call `publish()` in the native
coordinator's complete callback. Destroy it before releasing admission on skip
or failure. Publish is idempotent; destruction cancels unresolved reservations.
Do not suspend, reenter DB commands or carry an unresolved object across EXEC
scopes. A scope generation and pending-entry checks detect invalid publication.

Source and target observer sets form one union by observer identity. The shared
replication watcher is prepared once. If any observer declines or allocation
fails, every earlier prepared object is destroyed. Observers that do not support
family ownership explicitly decline; root-only SET/DEL fallback is forbidden.

## Replication preparation

Standalone MOVE preparation captures the immutable intent, complete one-entry
EXEC group, message and publication shell, then reserves encoded queue budget.
Changing the caller's strings afterward cannot change the intent. The native
transfer begins only after this succeeds. Cancelling releases the reservation.

Inside EXEC, page-only capture retains its existing vector and encoding. The
first prepared MOVE builds an ordered vector of references to prior page batches
and the new intent. It replaces the old vector only at publication. Later MOVE
preparation grows capacity geometrically; publication appends without allocating
an entry. Later page deltas join the same ordered vector. No collection pages
are copied into the intent or enumerated to construct it.

Wire budget charges the first event's 56-byte/two-record group envelope once.
Capture separately checks normalized workspace consumption: each MOVE costs
`64 + 2 * key bytes + root bytes` and three staging records. Page batches retain
their existing measurements. The limits agree with the receiver's 64 MiB and
65536-record caps. These budgets do not measure allocator overhead or RSS.

A declined preparation leaves earlier EXEC entries usable and records no MOVE.
This preserves command-local error handling rather than rolling back earlier
commands. After physical publication, failure to queue or finish a group must
invalidate subscribers. Existing end-of-EXEC wrapping and mailbox internals may
still allocate/fail; preallocation is not a claim that the entire native commit
and notification path is infallible. A stopped publisher discards the prepared
event, releases its charge and closes the affected subscribers.

## Validation and remaining integration

`move_publication` exercises paired-observer deduplication/rollback, allocation
failure, cancellation, queue limits, native preparation failure, stop during
publication, and normalized byte/record limits. It also runs real native memory
transfers through the publisher, subscriber, wire reader and receiver. These
cover standalone groups and page edits before/between/after two moves, immutable
snapshots, native page-address retention, version-3 rejection for moves and
version-3 compatibility after a cancelled move. oneTBB runs the same live paths
when compiled in. These are library-level transfers, not frontend MOVE tests.

The [public adapter](MOVE.md) now selects and validates DBs, applies source/target
expiry policy, integrates dispatcher metadata and ACL keys, declines the old
single-DB transaction workspace, and binds the durable journal for disk moves.
Disk mixed-group replay remains unsupported until a complete durable group
decision/recovery design exists. No source-capture test establishes PSYNC or
full synchronization.

Eight macOS configurations completed all-target builds and 130 selected CTests
on the 678-source snapshot. The new live fixture uses MDB and oneTBB, including
when compiled in a disk configuration; it does not execute a frontend MOVE or
a disk source transfer. Existing native EXEC/page-sequence tests cover the
unchanged page-only publication path. The preceding full coordinator fault
matrix was not repeated. See the [validation report][validation] for exact
selections, source epochs, failed compiler detection and retained artifacts.

[validation]: benchmarks/move-source-capture-validation-2026-11-02.json

See [ordered transport](ORDERED_REPLICATION.md),
[integration plan](MOVE_INTENT_INTEGRATION.md) and
[stages and estimates](ROADMAP_STATUS.md).
