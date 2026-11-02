# Packed List operations over native pages

`db/list_store` implements typed List operations inside `db::transact_pages`.
It supports packed two-ended pushes/pops, metadata length, indexed/range reads,
streaming position search, trimming, expiry updates, middle replacement/insertion/
removal, atomic moves, independent cloning and atomic replacement. Ordinary
nonblocking Redis List commands now use `dao::paged_list`. On native `mdb`,
new Lists use bounded pages and legacy JSON Lists migrate atomically on first
List DAO access. Unsupported engines fail explicitly before the callback;
there is no whole-value fallback. Hash and Set commands use bounded Map DAOs;
ZSet now uses its coordinated native member/score indexes.

## Layout and operations

New roots use `PLR` version 2: 48 bytes containing the existing expiry, element
count and head/tail ids, followed by the highest allocated page id. Existing
40-byte version-1 roots remain readable and acquire a counter by one page-chain
walk before their first allocating mutation. Counter exhaustion fails without
wrapping or publishing partial data. Empty Lists remove their root/pages.

Each physical page remains at most 16384 bytes, including headers. Small members
share pages; individual large members are fragmented across as many pages as
needed. An endpoint append loads one boundary page and emits changed/new pages,
not an encoded copy of the List. Batched pushes keep the current page decoded
until it fills, instead of re-encoding it for every small member.

Pop consumes fragments from the chosen endpoint. It assembles only requested
return values; discard/trim skips their value buffers. Range/index select the
nearer endpoint and skip unwanted members without copying their complete bytes.
Position search compares matching fragments directly against the target and
never allocates a buffer for a complete stored member. A caller requesting a
whole range still receives owning returned elements, as required by that request.
No stored whole-List blob or concatenated serialized List buffer is created.
The context is not copyable; it must finish before leaving the callback. An
unfinished mutation or failed operation poisons the transaction, preventing
page-only commits even if a caller forgets to finish or ignores an error.

Expiry updates change root metadata only. The store preserves exact deadlines;
the command layer remains responsible for clock checks, option precedence and
expiry/deletion policy. Typed initialization explicitly discards the previous
page family. The caller must check type/expiry or perform an authorized replacement
before invoking it; `open` reports nonpaged input instead of converting it.

Pages are validated individually and across visited links. Traversals reject
missing pages, invalid ids, cycles, bad neighbor links, inconsistent fragment
offsets/totals and declared members above the Redis 512 MiB member bound. Length
reads trust bounded root metadata and do not audit the whole page chain. These
checks are not checksums or durable corruption recovery.

## Common-key metadata integration

DAO reads use `record_codec::decode_stored`: the native `PLR` signature is
recognized before delegating other payloads to the configured legacy codec.
Both root versions expose List type, count and expiry without loading pages.
The decoded view marks its payload as paged and rejects whole-payload access
and opaque-byte validation; a root alone cannot safely represent a copied List.
Truncated, empty, invalid and future roots retain explicit error handling.
The legacy codec's `decode` entry point and encoder remain unchanged.

TYPE, EXISTS, key SCAN filtering, TTL reads, wrong-type checks and SET conditions
therefore recognize paged Lists. TTL mutation and PERSIST use `db::update_root`
under the same logical-key lock. A metadata-only write preserves its page family
and publishes a page transaction with a root write and zero page changes.
No page values are loaded or rewritten. The root's version, counters and exact
absolute expiry survive this path; expiry condition checks run before encoding.
Missing native page support or an incompatible observer declines preservation
before committing. Ordinary JSON expiry writes keep their original observer
format and whole-value replacement behavior.

Immediate/lazy expiry, DEL and authorized String overwrites reclaim the old
family through the existing deletion/replacement path. SET NX respects an
existing paged List, SET XX KEEPTTL preserves its deadline, and MGET returns nil
for its non-String value. COPY/RENAME detect paged sources under the legacy key
lock and use a native page-copy transaction. They recheck the source after reacquiring the locks and
retry if another command changed its representation. COPY uses independent page
families, including across logical DBs. RENAME clones and clears the source in
one transaction; native family-transfer optimization remains future work.

`paged_key_metadata` checks both root versions and their metadata-only views,
TYPE/EXISTS/PTTL/SCAN/MGET, wrong types, expiry conditions, PERSIST, DB 7 observer
routing, zero physical page I/O for metadata operations, page-aware key
transfer, String replacement, deletion, lazy expiry and corrupt/future root
retention. Failure fixtures cover native metadata write errors, incompatible
observers and engines without page support: all retain the exact root and its
members. Members include empty/binary values and a fragmented 40000-byte value.

## Middle edits and transfers

The cursor can report a member's first/last fragment addresses without building
its value. Replacement locates the chosen index from the nearer end; insertion
compares fragments until the first matching pivot. A splice retains the two
boundary pages' prefix/suffix, streams the new member into bounded pages, removes
superseded pages and patches changed neighboring links. Untouched List pages are
not rewritten. Prefix fragment addresses remain stable for earlier pending edits.

Removal compares values without full-member buffers and records adjacent match
runs as scalar addresses/counts. It then erases runs from the tail toward the
head, keeping earlier addresses valid. An all-match removal or an empty retained
trim range stages root deletion directly instead of sending every old page as
an individual deletion. Reclamation still costs native time proportional to the
old page family; the operation does not serialize those members.

Moves require one shared page transaction, making source pop and destination
push atomic. Existing source/destination deadlines stay attached to their own
keys. Same-key moves either read the endpoint unchanged or rotate it. Failure
at the destination poisons the entire transaction, preserving the source.

Cloning streams physical pages into a separate owner namespace, preserving exact
expiry and ids while atomically resetting the destination's previous family.
It verifies the full reachable chain and fragment/member totals. Mutable pages
are never aliased between owners. Copying a large value does not require one
large decoded member or whole-List buffer.

`db::copy_pages_to` coordinates same-DB and cross-DB copying. DB/key locks follow
a total order, including opposite-direction copies. The callback may read the
source and mutate only the destination. Source root actions, bytes, reset flags
and page changes are checked before commit; any source modification aborts the
copy. Only destination pages/metadata are committed and published using its DB
identity. Native page support is required on both engines. This is a copy
primitive, not a general two-DB write transaction or an implementation of the
Redis COPY command's option/error policy.

## Atomic replacement

A root entry can request `clear_pages` with its metadata write. The public
`reset_pages(owner)` staging operation also removes any earlier staged page
changes for that owner and hides its old pages from later reads. It requires
an explicit surviving root write. New List creation and reinitialization use
this boundary, so reused page ids cannot inherit stale pages.

Native commit temporarily detaches each affected old page map by a nonallocating
swap under all affected shard locks. It journals new insertions as usual. Failure
restores the exact old maps and roots; success publishes metadata/new pages and
reclaims detached old pages. This does not copy an entire old page index or its
payloads. Reclamation remains synchronous and can take time for large owners.

PKV2 operation 12 (ROOT_REPLACE) carries this reset with its root metadata. It
is measured and queued with the transaction, staged by the receiver and applied
only at COMMIT. Old pages disappear atomically with replacement metadata/new
pages. Existing ROOT_WRITE continues to preserve untouched pages. Older PKV2
implementations without ROOT_REPLACE reject it; there is no reconnect replay.

## Evidence and remaining work

`list_store` checks 10000 packed small members, endpoint access counts, 400
randomized operations against an independent deque, binary/empty members,
page boundaries and a 1 MiB fragmented member. It checks reverse reads/pops,
ranked positions, trim and a large-member search with no allocation of 32 KiB or
larger. Expiry updates perform no physical page reads/writes. Four threads each
run four fibers with explicit yields inside push/pop transactions.

Allocation injection sweeps staging and native replacement commit. Every failed
attempt must retain the original root bytes and complete member sequence.
Corruption/counter-exhaustion fixtures reject unsafe traversal/allocation.
`page_replication` captures actual List-store events, transmits ROOT_REPLACE,
applies them to an independent DB and verifies expiry, members and old-page
reclamation. The real-process `page_live_stream` also includes ROOT_REPLACE.

The page DAO preserves lookup/type/index-error precedence, missing-key behavior,
request order for LMPOP, independent key TTLs and atomic two-key moves. Legacy
migration reads selected elements into small batches, retaining the original
parsed document only during import. It never builds a second vector containing
all members. A failed command or commit aborts migration together with its edits.
Even LLEN can trigger a first-access migration; native LLEN reads metadata only.
The legacy JSON DAO remains available for old-codec fixtures, not server routing.

`paged_list_dao` compares 600 operations/results/errors with the independent
legacy DAO, inspecting every surviving native root's bounded shape. It migrates
1001 members including a 1 MiB member, checks exact TTL and a rejected command's
rollback, and exercises independent COPY, RENAME/NX, LMOVE and LMPOP precedence. A 400-copy race checks source representation
changes among String, JSON List and native List without partial snapshots.
Future physical page versions also retain their unsupported-format error
classification through COPY/RENAME, without changing either key.
Existing socket command/cluster tests now execute this page-aware DAO. Real
subscriber tests inspect bounded List frames and root-only TTL events; a real
source/receiver pair verifies fragmented members, DB 7 copies, RENAME and LMOVE.

Native `mdb`, `mdb_tbb`, single-instance `rdb` and `wdb` supply page transactions.
Older memory engines need implementations; structured requests fail explicitly
there rather than write whole serialized collections. Disk cross-path mutation
limits are described in `ROCKSDB_STORAGE.md` and `WIREDTIGER_STORAGE.md`. Hash
and Set use bounded Map DAOs. ZSet uses native member/score indexes. Initial
replication sync, replay, resource/durability guarantees, blocking List commands
and broader Redis coverage also remain open.

`list_edits` adds 500 vector-model steps, locality assertions on a 10000-member
List, 1 MiB edits with no allocation of 32 KiB or larger, all direction pairs of
moves, independent cloning, 80 identical-key opposed copies, 80 distinct-target copies and source-write
rejection.
Allocation sweeps cover replacement, moves, cloning and cross-DB copying. A
failed destination allocation after popping the source must leave both Lists
unchanged. Replication fixtures route middle, move and DB 7 copy events through
the actual frame receiver and verify metadata, page ownership and reclamation.
