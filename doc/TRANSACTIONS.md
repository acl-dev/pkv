# MULTI, EXEC, DISCARD, WATCH and UNWATCH

Connections own binary-safe command snapshots between MULTI and EXEC. Commands
reply QUEUED and make no data changes before EXEC. EXEC returns one array with
one reply per command, including nested arrays, nulls and runtime errors. A
runtime error does not roll back earlier writes or prevent later commands from
running. Unknown commands and invalid command arity dirty the queue; EXEC then
returns EXECABORT without executing it. Nested MULTI and WATCH inside MULTI
return errors without dirtying an otherwise valid queue. DISCARD releases the
queue and watched keys. QUIT closes the connection immediately; queued commands
are discarded on disconnect.

SELECT executes in queue order. Its successful selection persists after EXEC.
WATCH tracks keys in the selected database; changing databases retains earlier
watches. Repeated WATCH of an already watched key does not refresh its snapshot.
UNWATCH outside MULTI releases all watched keys. Inside MULTI it is queued.
EXEC, including an optimistic conflict or EXECABORT, and DISCARD release watches.

## Isolation and expiry

Ordinary commands retain concurrent shared admission. EXEC closes admission,
waits for in-flight commands and guarded storage operations, then holds exclusive
admission until every queued command has produced its reply. Socket output occurs
after admission is released. Storage guards are reentrant for the current fiber;
an unrelated fiber on the same OS thread cannot borrow another fiber's exclusive
admission. Reader counts belong to threads, avoiding a shared reader counter on
every ordinary command. ACL fiber mutexes and condition variables coordinate the
waits across workers. The single-thread-only ACL fiber_rwlock is not used here.

Expiry checks inside EXEC use its admission-time clock snapshot. A key that was
watched while live and expires before EXEC invalidates the watch even if no
other client touches it. WATCH of an already expired key is a clean missing-key
snapshot; later lazy deletion of that expired record does not invalidate it.
A conflict returns a RESP2 null array and executes no queued commands.

Watched-key epochs live in a striped registry. Committed mutations invalidate an
epoch under the same key lock used for registration. Unwatched stripes skip
registry locking. Subscriptions remove registry entries when their last observer
leaves; invalidated epochs remain owned only by connections still observing them.
Failed commits do not invalidate watches. Native page commits, ordinary root
writes, cross-database COPY destinations and replica applications share these
hooks. The execution gate also covers replica storage operations.

Logical write semantics matter: same-value SET and HSET, empty APPEND, full-range
LTRIM and same-end self LMOVE invalidate WATCH. Failed NX, missing DEL, duplicate
SADD, unchanged ZADD, read-only commands and unchanged bitmap writes do not.
Successful explicit GETEX expiry invalidates even an unchanged deadline; PERSIST
of a persistent String does not. Pure legacy JSON-to-native migration is marked
as maintenance and does not invalidate a watch of an existing root. Applying a
complete migration image to an absent replica root creates a key and invalidates
a missing-key watch. Subsequent actual changes in the
same command clear that marker before commit.

## Cluster checks and limits

A queued command's keys are checked before QUEUED. CROSSSLOT, CLUSTERDOWN or MOVED
dirties the queue. EXEC checks the slots of all queued commands together and
rechecks the destination owner before starting any command. Commands that each
use a valid local slot can therefore queue successfully but fail EXEC with
CROSSSLOT if their slots differ. This does not implement cluster migration,
ASK handling or automatic failover.

Each connection may queue at most 65536 commands and 64 MiB of accounted command
arguments and vector/string metadata. Capacity slack and response buffers are
additional memory. These are pkv limits, not Redis's global maxmemory policy.
A queue limit/allocation error dirties the transaction and leads to EXECABORT.
The RESP parser's existing size limits still apply. Once WATCH is dirty,
subsequent commands receive QUEUED without retaining more snapshots. A later
allocation failure during execution or response building closes that client
connection; earlier committed writes remain and the execution gate is released.

Source EXEC now stages one referenced native logical DB with one atomic
physical commit domain and commits successful commands once, before success
replies. See [source commit integration](SOURCE_EXEC.md) for SELECT/COPY routing,
failure responses and process-crash tests. Cross-DB, multiple-physical-store,
opaque legacy COPY/RENAME and non-native fallbacks still commit commands
separately and can retain a crash prefix. [PKV3 replication](EXEC_REPLICATION.md)
publishes whole EXEC groups and applies single-database groups in one native
destination commit; mixed-DB groups reject before any replica write.
Cross-database recovery, initial synchronization, replay and Redis replication
compatibility remain open.

ROOT_MAINTENANCE (PKV2 operation 13) preserves the distinction between format
migration and user writes on a new receiver. Older v2 binaries reject this
unknown operation and disconnect before applying its group. Upgrade both ends;
the existing v2 negotiation does not advertise this opcode independently.

## Verification

Command names containing NUL are rejected as unknown, including transaction
controls. This is a deliberate strictness difference from the local reference,
which accepts some suffixed names. Keys and values remain binary-safe.

The socket suite runs the same protocol, WATCH semantics and optimistic CAS /
concurrent coherent-read scenarios against pkv and the available local Redis
8.6.2 reference, which reports modified=1. It is not stock-release certification.
Additional pkv-only cluster checks cover rejection before writes. Unit fixtures
cover four worker threads with multiple fibers, reentrant guards, frozen EXEC
time, owned queue snapshots, command/byte limits and allocation failures during
queue capture and response ownership transfer. Native WATCH tests cover all
five legacy migrations, invalid metadata, missing-key epochs and replica commits.
The page-stream fixture verifies maintenance opcode capture and wire decoding.

An [ordered page replay primitive](PAGE_SEQUENCES.md) now stages a database's
page batches and commits them once, preserving prefix validation and WATCH
history. It now serves the live single-database EXEC stream; cross-DB recovery is open.

[Native page workspaces](PAGE_WORKSPACES.md) now serve the eligible live EXEC
path. Cross-database/physical-store coordination and the remaining legacy
fallbacks require further integration before every EXEC can claim one durable
source commit.
