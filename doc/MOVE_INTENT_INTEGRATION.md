# MOVE and ordered replication integration plan

Protocol version 4 implements bounded ordered transport and receiver replay.
Prepared source observation and ordered capture are now implemented at the
library boundary and connected to the [public MOVE adapter](MOVE.md).
Disk-backed mixed MOVE groups still require a durable group decision design.
See [the wire contract](ORDERED_REPLICATION.md) for the implemented boundary.

## Verified starting points

- `redis_key` exposes MOVE with exclusive dispatcher admission, command
  metadata, ACL key extraction and source/target expiry policy.
- `exec_batch` owns page batches or an ordered vector of immutable page/MOVE
  entries. Its base envelope costs 56 bytes and two records. Page-only groups
  retain their original representation and minimum protocol version 3.
  `slave_watcher` preserves this fast path until the first MOVE is published.
  Its prepared source API records page/MOVE entries in command order, with
  cancellation and separate wire/workspace quota checks. See
  [source capture](MOVE_SOURCE_CAPTURE.md) for the ownership contract.
- `family_move` already holds source/target DB indices, the same key, exact raw
  source root, target presence and raw target root. An empty root is distinct
  from absence. `family_workspace` stages page batches and these moves under
  receiver-only exclusive admission; any failed stage poisons the group.
- The family workspace contract explicitly forbids holding its admission while
  waiting for network frames. It is not a primary-side command workspace.
- Existing page batches cap total bytes at 64 MiB, records at 65536, encoded
  key bytes at 65525 and database indices below 65536. These are existing
  private-stream limits, not proof of full Redis key-size compatibility.
- The publisher recognizes explicit versions through 4. The receiver first
  requests 4 and can reconnect with 3, 2 and 1. Authentication rejection does
  not initiate this downgrade. An old peer rejects a MOVE group before any
  earlier message or group prefix is sent.
- Active expiry is not started when an upstream replica source is configured.
  Registered replica read-only commands enter `replica_read_guard`; observed
  DB mutation APIs discard their proposed cleanup while retaining logical
  read results. The existing retention stream fixture tests delayed updates
  after expired reads. New intents must preserve this protection and must not
  reinterpret authoritative source decisions using a replica clock.

## Required invariants

1. Parse and validate the complete bounded ordered group before constructing
   the admission-owning receiver workspace. A stalled connection must not hold
   command admission or block process shutdown.
2. Represent each entry as an immutable page batch or bounded MOVE intent in
   original command order. Never replace a collection transfer with root-only
   delete/write events: that loses ownership of the source's existing pages.
3. Encode DB indices, same key, exact raw prefix roots and target presence.
   The receiver checks these against its staged prefix state, without a local
   expiry-clock decision. Preserve binary keys and empty-root distinctions.
4. Charge every envelope, intent, raw root, key and record with checked
   arithmetic. Reserve publication ownership and queue budget before primary
   publication, then keep the captured data immutable until sending completes.
5. Preserve primary EXEC's command-local errors and commits. Receiver group
   poison/atomic publication must not turn primary EXEC into rollback-all.
6. Keep the old byte encoding for page-only versions. Require explicit new
   negotiation for MOVE intents. Unsupported peers must not receive a lossy
   downgrade or a committed prefix of a group.
7. Keep native family transfers within supported engine capabilities. Explicit
   new page deltas do not imply arbitrary native-container interchangeability.
   No fallback may serialize the entire collection into one large value.
8. Disk ordered groups need a durable decision/recovery design covering every
   touched DB and family. Existing standalone MOVE journaling is not, by
   itself, proof of multi-command group crash atomicity.

## Implementation sequence and acceptance

The immutable entry model, framing, measurement and complete-group assembly
are implemented. C++ fixtures cover literal MOVE bytes, old page-only byte
equivalence, malformed frames, allocation failure and old-peer rejection.

Complete mixed groups now enter the existing memory family workspace only
after COMMIT. Tests cover native cycles, page deltas before/between/after moves,
empty/raw roots, prefix mismatch and native page address retention. Real TCP
tests cover five paged types, fragmented prefixes, rejection, reconnect and a
stalled uncommitted group. This does not establish disk group durability.

Prepared capture is now connected at the public MOVE boundary. The adapter
validates database arguments, cluster restrictions, same DB, absent/expired
source and occupied destination. The old single-DB transaction optimization
declines MOVE, preserving visibility of earlier EXEC writes. Native transfers
use the bound journal, defaulting to auto, and publish paired local effects.

The collector now charges the 56-byte/two-record envelope on the first entry,
including when a MOVE precedes all page deltas. Queue epoch and encoded budget
belong to the prepared publication, so a skipped/failed move releases its
reservation without leaving an intent in the EXEC group.
`mbox::push` returns failure; preallocating a `publication` alone does not prove
enqueue cannot fail. Preserve explicit subscriber invalidation on publication
failure and never treat a successful local transfer as replicated merely
because its immutable intent was allocated. Subscriber registration is already
linearized by `replica_scope` exclusive admission.

Public command fixtures now exercise real frontend moves, TCP replication into
a memory replica, active expiry, legacy-peer invalidation and native restart.
The online native interruption fixture observes a pending decision during an
actual frontend MOVE before stopping and killing its owned process. Retain
this evidence separately from library fixtures; disk mixed-group durability
and Redis synchronization remain outstanding.

See [replica read retention](REPLICA_READ_RETENTION.md),
[ownership replay](OWNERSHIP_REPLAY_VIEW.md),
[source EXEC](SOURCE_EXEC.md), [MOVE recovery](MOVE_RECOVERY.md) and
[stages and estimates](ROADMAP_STATUS.md). The full Redis goal remains active.
