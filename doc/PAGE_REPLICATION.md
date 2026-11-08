# Bounded page-transaction stream components

The PKV2 framing described here remains supported. Current negotiation and the
bounded grouped-erasure extension are documented in
[compact erasure replication](COMPACT_ERASE_REPLICATION.md).

Page transactions now flow through subscriber negotiation, the common publication
FIFO, bounded per-subscriber queues and the live receiver loop. The source uses
native atomic root/page commits; the receiver applies only completed transactions
without observer echo. Ordinary List commands now publish bounded pages through
this path. This transport does not provide a snapshot.

## Framing

Each frame starts with five network-order u32 fields (20 bytes):

| Field | Meaning |
| --- | --- |
| Magic | `0x504b5632`, the bytes `PKV2` |
| Operation | One operation from the table below |
| Database | Logical database index, 0 through 65535 |
| Key bytes | At most 65525 bytes, including empty/binary keys |
| Payload bytes | Operation-specific bounded length |

The key and payload follow the header. A transaction is BEGIN, all root
declarations sorted by binary key, all page changes sorted by `(owner, id)`,
then COMMIT. There is no interleaving or nesting. Every frame carries the same
logical database index.

| Operation | Number | Key | Payload |
| --- | --- | --- | --- |
| BEGIN | 5 | Empty | u32 root count, u32 page count, u64 total wire bytes |
| ROOT_KEEP | 6 | Owner | Empty; declare existing root without rewriting it |
| ROOT_WRITE | 7 | Owner | Root metadata, at most 16384 bytes |
| ROOT_ERASE | 8 | Owner | Empty; remove root and all its pages |
| PAGE_WRITE | 9 | Owner | Nonzero u64 page id, then at most 16384 page bytes |
| PAGE_ERASE | 10 | Owner | Nonzero u64 page id only |
| COMMIT | 11 | Empty | Empty |
| ROOT_REPLACE | 12 | Owner | Metadata; clear previous pages at commit |
| ROOT_MAINTENANCE | 13 | Owner | Replace pages without a logical mutation |

BEGIN counts include root declarations/page changes. The wire-byte total includes
all headers, keys, payloads, BEGIN and COMMIT. All payload integers use network
byte order. A page id prefix adds 8 wire bytes without increasing the physical
page-value bound. A long owner key can make a complete frame larger than 16 KiB;
the physical page payload itself remains bounded.

Every page owner must have exactly one earlier root declaration. ROOT_REPLACE has the same metadata bound as ROOT_WRITE. Root and page
addresses are unique and strictly ordered. A root KEEP with no page changes is
omitted by capture and rejected as a noncanonical received transaction. Page
writes cannot accompany root deletion. ROOT_KEEP does not create an absent root;
native apply rejects a page write whose destination root is absent.

## Limits, ownership and application

The hard transaction limits are 64 MiB encoded wire bytes and 65536 frames,
including BEGIN/COMMIT. At least one root declaration is required. BEGIN counts
and minimum byte charges are validated before reserving staging vectors. Counts,
actual bytes, per-frame lengths, database identity, ordering, ids and the complete
structure are checked before a completed immutable batch is returned.

`page_batch::measure` validates/counts without allocating or copying payloads.
Capture produces an owning immutable batch, omitting unrelated read-only roots.
The sender writes one bounded frame with scatter/gather I/O at a time. It does
not concatenate all values into one serialized transaction buffer. The receiver
stages owning changed records under bounded frame/count/byte limits; it never
creates one combined whole-List value. Bounds charge encoded data, not total RSS
or connection count. Transaction staging still uses memory proportional to all
changes in the transaction.

Only COMMIT can return a complete batch. Pending, truncated and malformed input
cannot touch a database. An invalid sequence clears staging and poisons the
receiver until reset. The connection owner must close a failed stream and reset
its receiver on reconnect; it cannot resume inside a partially written frame or
transaction. The standalone codec deliberately does not manage connections.

`page_batch::apply` invokes `db::replica_apply_pages`, which uses the same ordered
logical key locks and native root/page commit as local page transactions. It
bypasses observer publication to avoid replication loops. Failed lookups,
allocation or native commit leave all destination roots/pages unchanged. Engines
without native page support decline the apply. The caller must resolve
`batch.database()` through its configured logical DB pool before applying; the
codec does not route to DB 0 or guess a destination.

ROOT_KEEP and ROOT_WRITE preserve pages not named by the transaction.
ROOT_REPLACE atomically drops the old family before installing new pages. Repeated application of
a complete unchanged batch is idempotent, but this does not supply transaction
ids, offsets, ordering across reconnects, acknowledgements or replay recovery.
The format does not validate typed List links, member totals or codec versions;
those remain the typed storage layer's responsibility.

## Live negotiation and queues

New receivers request `PKV-REPL 2\r\n`. The server queues READY with PKV2 framing
behind earlier legacy data, then emits ordinary and page frames using PKV2.
Identical repeated requests do not queue another READY; changing the requested
version on an existing subscription closes it. Ordinary SET/GET/DEL/READY retain
operation numbers 1/2/3/4 and their existing limits in the PKV2 header.

If the upstream closes before acknowledging a v2 request, the receiver retries
v1 on the next connection. A later reconnect tries v2 again. It accepts a lower
acknowledged version, but never a version higher than requested. Legacy DB zero
traffic can still arrive from an upstream ignoring negotiation. READY and pending
page state are reset on every connection. Version changes after READY, duplicate
READY and ordinary records inside a page transaction close the stream.

One immutable owning batch is one publication/queue unit, so its frames cannot
interleave with other updates. The publication FIFO retains the logical key lock
ordering of source commits. Queue byte charges include all encoded frames;
message charges count actual frames, including BEGIN/COMMIT. All charges remain
until forwarding/sending the entire group completes. Publication reserves the
budget before cloning payloads. Limits and overflow behavior use the existing
replica queue settings described in [Replication](REPLICATION.md).

A page-capable observer dispatcher checks format limits before native commit.
Unsupported observers or unrepresentable transactions are rejected without a
write. After commit, publication/output overflow closes affected subscriptions;
the source write remains committed. Legacy DB zero and v1 subscribers close when
a page transaction they would otherwise need is published. Legacy subscribers
continue to filter DB N records, including page transactions, without closing
their DB zero stream. These rules avoid retaining a stream with omitted updates.

The live receiver dispatches frames to one connection-local receiver, rejects
invalid/unconfigured DB indexes and uses the selected DB for atomic application.
Only complete transactions can reach native storage. A framing, apply or I/O
failure drops pending state and reconnects; it does not replay the lost suffix.

Initial synchronization, offsets, acknowledgements, replay, authentication,
codec negotiation and disk recovery remain open. Sustained PKV2 load, general
allocation exhaustion, connection/global RSS budgets and large-delete latency
still need measurement and policy. List traversal, mutation, common-key handling
and legacy migration remain required before List commands can use these pages.

## Verification

`page_replication` exercises actual socketpair transport, independent exact BEGIN
bytes, empty/binary keys, maximum page ids, ROOT_KEEP, deletes, owning capture,
per-byte fragmentation, every truncated byte of a representative transaction,
invalid frame headers, illegal sequence/order/DB/count/byte combinations,
poison/reset behavior and the exact record-count cap. Allocation sweeps cover
capture, receiver staging and native replica apply. Pending input never changes
the destination and failed apply preserves all prior data; observers see no
replica SET/DEL echo.

An additional stream exceeds the legacy 1024000-byte value limit using 80 bounded
List pages. Its root is 40 bytes with an exact expiry, and its single member is
fragmented across pages. The test routes the completed DB 15 batch through a
logical DB pool, decodes the resulting root/page and verifies DB 0 isolation.
`live_page_replication` additionally connects actual native source commits,
observer publication, subscription negotiation, bounded queues, wire dispatch and
cluster-client application to independently inspected destination pages. It
checks exact/full frame charging, ordinary updates after a group, DB N routing,
legacy filtering and v1 disconnection, plus byte/count overflow in both queues.
A stalled large-group send remains fully charged until failure; overflow wakes
the writer, drains references and cannot apply the received incomplete prefix.

`page_live_stream` drives an actual pkv process from a TCP upstream and tests
uncommitted data invisibility, failed ROOT_KEEP atomicity, reconnect discard,
v1 fallback, interleaving rejection, oversized BEGIN, DB isolation and duplicate/
changed subscriber requests. Its roots use JSON string fixtures so existing
commands can inspect committed metadata; physical page contents are independently
checked by the native live-path fixture. No List command storage switch or full
Redis replication parity is claimed.

Middle List edits and moves now have component replication fixtures as well.
They pass captured source events through frame decoding and cluster-client
routing, then read native destination Lists. A cross-DB clone publishes only the
DB 7 destination event, preserves its source event stream and replaces old
replica pages. Ordinary List commands now use this path; see `LIST_STORAGE.md`.


Default String writes now use native PSR transactions, including inline values
without data pages. PKV2 retains those transactions. A v0/v1 peer may receive
page-free inline String roots as bounded JSON SET/DEL events, charged by their
actual encoded sizes. Data pages, other native formats, allocation failure or
output-budget exhaustion disconnect that peer. Legacy per-root events do not
preserve multi-key PKV2 atomicity. See `STRING_PAGES.md`.

ROOT_MAINTENANCE uses ROOT_REPLACE bounds and resets the page family. It
preserves WATCH across pure legacy format migration. Ordinary writes clear the
maintenance marker. Older v2 receivers reject opcode 13 before applying its
group; upgrade both endpoints. There is no separate opcode capability bit.
Whole-EXEC publication now uses [PKV3](EXEC_REPLICATION.md). Complete
single-database groups apply through [ordered page replay](PAGE_SEQUENCES.md)
in one destination commit. All-path source whole-EXEC durability, cross-database commits
and reconnect recovery remain open.

Eligible native source EXEC groups now stage and commit once before success
replies. See [source commit routing and remaining fallbacks](SOURCE_EXEC.md).
Cross-database/physical-store and legacy whole-value key-operation paths still need
coordinated recovery or migration.
