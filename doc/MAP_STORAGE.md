# Bounded field/member storage

`db/map_store` implements a native ordered field index inside
`db::transact_pages`. It is the storage component for replacing whole-Hash JSON
writes, with a Set kind available for empty membership payloads. Existing Hash
commands use `dao::paged_hash` on native memory and single-instance disk
backends, including metadata/key operations, atomic legacy migration and PKV2
replication. Set commands use `dao::paged_set` on the same backends; see [SET_STORAGE.md](SET_STORAGE.md).
ZSet now uses coordinated native member/score indexes; see `ZSET_STORAGE.md`.

## Layout

Native pages belong to the existing `(logical key, uint64 id)` family. No
prefix in the user key space is reserved, and physical pages never appear in
key SCAN. All integer fields use network byte order.

| Record | Magic | Size | Contents |
| --- | --- | --- | --- |
| Root | PMR v2–v5 | 48 bytes (v2/v3), 12 or 20 bytes (v4/v5) | Kind, expiry, count, tree id, highest id |
| Directory | PMD v1 / v2 / v3 / v4 | 4112 / 272 / 72 / 13–58 bytes | Bucket roots and counts |
| AVL node | PMN v2–v9, v11–v18, ZL, 0xf4–0xf5 | 80–272 bytes (v2), variable for packed nodes | Id, bucket, children, height/count, two blob descriptors, inline data |
| Blob fragment | PMB v1 / v2 | 33–16384 / 9–520 bytes | Id, next id, byte offset, nonempty payload |

The root kind is Hash or Set. Exact millisecond expiry is stored as a signed
64-bit deadline, with `-1` for persistence. Count is bounded by signed 64-bit
range. Empty maps remove their root and native family. The monotonic id counter
is shared by directory pages, tree nodes and blob fragments; exhaustion fails without wrapping.
Other page versions return an unsupported-format status; reserved bytes must
remain zero.

PMN v7 stores leaf nodes with 12–14 inline key and value bytes and a
16-bit page ID in a 20–22-byte record. The eight-byte header contains the
signature, bucket, two four-bit lengths, and page ID. Leaf height and count
are both one.

PMN v8 stores eligible branches with a 14-byte inline key, bucket zero and
an empty value in 22 bytes. The eight-byte header contains the signature,
a packed height and subtree count, and three one-byte node IDs. This
representation supports heights 2–9 and subtree counts 2–32.

PMN v9 uses a 16-byte header for inline nodes whose ID, child links and
subtree count fit in 16 bits. PMN v3 remains available for wider inline
nodes, and PMN v2 stores nodes with external blobs or 64-bit IDs.

PMN v11 uses an 11-byte header for nodes with a 15–20-byte inline key and
an empty value. It stores the bucket, height and count (up to 8 and 32)
and packs the page ID and both child IDs into three 12-bit fields. Upgrade
replication readers before sending pages encoded in a version they do not
support.

PMN v12 uses a seven-byte header for leaves with a 16-bit ID, a nonempty
inline value and 24–40 combined inline bytes. The header stores the key
length, and the decoder derives the bucket from the key and the value
length from the record length.

PMN v13 uses the v8 branch header with a six-byte inline key and an
eight-byte inline value. The decoder derives the bucket from the key.
The writer uses it for eligible hashed branches, including Sorted Set
member indexes. Replication readers must support v13 before receiving
these pages.

PMN v14 uses a nine-byte header for eligible inline leaves whose IDs exceed
16 bits and fit in 32 bits. The header holds the signature, page ID, key
length and a flag for a derived bucket. Hashed leaves with at least 24
combined inline bytes derive the bucket from the key and infer the nonempty
value length from the record length. Score
index leaves with keys longer than 14 bytes use bucket zero and an empty
value. Replication readers must support v14 before receiving these pages.

PMN v15 uses a 19-byte header for inline nodes whose page ID and child
links fit in 24 bits and whose subtree count fits in 16 bits. It stores
the same bucket, height and byte lengths as v3. Writers select it when
the smaller v9 format is unavailable and at least 24 inline payload bytes
are present. Existing formats remain readable. Replication readers must
support v15 before receiving these pages.

PMN v16 uses a seven-byte header for hashed leaves with a 16-bit ID, a
15–24-byte inline key and an empty value. It stores the bucket and derives
the key length from the record length. Replication readers must support v16
before receiving these pages.

Bulk Set builds on memory backends can write leaves that fit in the string's
inline storage. The three-byte header contains `0xf4` and a big-endian word
whose lower 14 bits store the nonzero page ID. The upper two bits store the
member length minus 16. The member follows the header and supplies the
bucket. Height and count are one, and child links and the value are empty.
Replication readers must support this format before receiving these pages.
`make PKV_COMPACT_SET_LEAF=NO` disables new writes in this format and retains
read support.

PMN v17 uses a six-byte header for leaves written directly during a bulk ZSet
build with compact ZSet encoding enabled. The two-byte word following the
version stores a 15-bit page ID and a flag for the member index. The member
index has a trailing eight-byte score and derives the bucket from the key.
The score index has an empty value and bucket zero. The inline payload is
15–25 bytes, and the height and subtree count are both one. The decoder
accepts earlier PMN versions. Replication readers must support v17 before
receiving these pages.

Bulk ZSet builds also use `ZL` leaves when the inline payload is 26 or 27
bytes and the page ID fits in 14 bits. The four-byte header contains the
two-byte `ZL` signature and a big-endian word. Bits 0–13 hold the nonzero page
ID, bit 14 selects the 27-byte payload, and bit 15 selects the member index.
The member index derives its bucket from the key and stores an eight-byte
score after it. The score index keeps bucket zero and an empty value. Both
layouts require the declared payload length and imply height and count one.
Replication readers need `ZL` support before receiving these pages. The
`PKV_COMPACT_ZSET=NO` build option disables new compact ZSet writes.

The memory backends also use a three-byte ZSet leaf header for inline payloads
of at least 28 bytes. The `0xf5` tag precedes a big-endian word. Its low 15 bits
hold the nonzero page ID and its high bit selects the member index. The member
index derives the bucket from the key and stores an eight-byte score after it.
The score index has bucket zero and no value. Key length is inferred from the
record size and is at most 127 bytes in either index. Bulk builds and point
updates can write this format with compact ZSet encoding enabled. Upgrade
page replication receivers before enabling these writes.

PMN v18 stores a 20-byte Set node whose member resides in fragments. After
the version come the bucket and height, then five 16-bit words for the node
ID, left and right child IDs, subtree count, and first fragment ID. A 32-bit
member length follows. The node value is empty. Writers use PMN v2 when
an ID or subtree count exceeds 16 bits.

PMN v19 stores a ZSet node whose key resides in fragments. After the
four-byte signature and version come flags, height, bucket and a
four-byte big-endian key length. Five big-endian fields follow for the
node ID, left and right child IDs, subtree count and first fragment ID.
Flag bit 0 selects four-byte fields instead of two-byte fields. Flag bit 1
marks the member index and adds an eight-byte score after the fields.
The score index uses bucket zero. Writers use PMN v2 when a field exceeds
32 bits. Upgrade page replication receivers before enabling v19 writes.
`make PKV_COMPACT_ZSET=NO` disables new v19 writes and retains read support.

PMN v20 stores a ZSet score node whose member bytes reside in the member
index's fragment chain. It uses the v19 header order. The flags byte is 0,
1 or 2 for two-, four- or eight-byte fields, and the bucket byte is zero.
The key length includes the eight-byte ordered score prefix stored after
the fields. The first fragment ID refers to the member's own chain, whose
offsets count member bytes from zero. Score nodes never free this chain.
Upgrade page replication receivers before enabling writes of v20 records.
For a mixed deployment, build the writers with `PKV_COMPACT_ZSET=NO` before
they create any v20 records. The disabled build can still read existing
v20 records and preserve their references during page edits.

The memory backends can use PMB v2 for a member of up to 512 bytes stored
in one fragment. The header is the four-byte signature and version followed
by a 32-bit page ID. The payload follows with an implied zero offset and no
next page. Readers accept both fragment versions and both node layouts in
one family. Upgrade page replication receivers before enabling v18 writes.
`make PKV_COMPACT_SET_FRAGMENTS=NO` selects PMN v2 and PMB v1 for fragmented
Set members.

Standalone Hash and Set roots use PMR v4 when their count and page IDs fit
in 16 bits. The signature and version occupy bytes 0–3, followed by kind and
an expiry flag. The count, tree ID, and highest page ID use two bytes each.
Expiring roots append the eight-byte deadline. The decoder accepts v2 and
v3, and the writer uses those layouts for larger or bucketed roots.
Replication peers must support v4 before receiving compact roots.

Each field uses one AVL node. Keys up to 64 bytes and values up to 128 bytes stay
inline in that node. Larger keys/values use separate fragment chains, each with
at most 16352 payload bytes and a 32-byte header. A descriptor stores total
length and first id, without a directory proportional to field count or size.
Individual keys/values are currently bounded at 512 MiB. Empty/binary keys and
values are supported. Set nodes require empty values.

Fields are ordered by `(stable scan bucket, field bytes)`, with 256 buckets
computed by the existing scan hash. The node stores its bucket in byte 4; bytes
5–7 remain reserved. Root/node version 2 distinguishes this order from the old
lexically ordered v1 prototype. V1 roots/nodes are explicitly unsupported,
including during clone; they are never silently reinterpreted. Blob fragments
remain version 1. The v1 prototype was not activated in server commands or a durable backend.
Small Hash commands create version 2 directly. When an unpartitioned Hash
adds its 129th field and sufficient ids remain, the same transaction converts
it to a grouped PMR root. PMR v5 uses the same compact fields as v4 while
the count and IDs fit in 16 bits. PMR v3 (layout byte 5 = 2) stores wider
grouped roots. The root points to a 16-entry
top directory. Each nonempty entry points to another 16-entry group.
That group's entries point to independently balanced AVL bucket trees. A point lookup
reads the top and its selected group, not all 256 bucket entries. Full traversal
validates every group. Counts, ids, reserved bytes, tags and local aliases are
validated on reads. Child nodes must match their selected scan bucket.

PMD v2 uses a 16-byte header followed by sixteen `(tree id, count)` pairs.
The header contains `PMD` and version 2, a two-byte group tag (256 for the top,
0 through 15 for groups), two reserved zero bytes and the page id. All fields
use network byte order. Empty groups have a zero top entry and no group page.
PMD v3 uses an eight-byte header with the same signature and tag followed by
the page ID. The page ID and both fields in each pair use 16 bits. Groups use the
compact format while their page ID bound and count fit in 16 bits. Readers
accept mixed formats within the same directory tree.
PMD v4 adds a 16-bit occupancy bitmap to the eight-byte header. Each
occupied entry stores a 16-bit tree ID and an eight-bit count. Groups
with larger counts use v3 or v2. Upgrade replication readers before
sending v4 pages.
Migration adds one top page plus one page per nonempty group, reuses node ids,
and leaves large blob fragments unchanged. Staged writes publish atomically;
allocation or storage failures abort the transaction. Hash shrinking retains this
layout until the final field removes the entire family.

Existing PMR v2 trees remain readable, including already-large trees. PMR v3
layout 1 with a flat 256-entry PMD v1 directory also remains readable and
writable. Set also uses the grouped layout on reaching 257 members. On compact
Set capable memory backends, a reducing Set command can replace a small
surviving native Set with a compact root and clear its child pages atomically;
see [SET_STORAGE.md](SET_STORAGE.md). Hash shrinking retains its grouped layout.
ZSet continues to use its own member and score roots. This is forward migration:
older binaries without PMR v3 support cannot read newly partitioned Hashes.
Bucket order and HSCAN cursor boundaries remain unchanged across layouts.

MapStore borrows directory scratch lazily from one fixed state per worker.
Small unpartitioned maps do not acquire this state. A live lease excludes
nested operations and other fibers on the worker; those use independently
owned fallback state. Every indexed read still fetches and validates current
transaction pages, including staged updates. This is temporary storage reuse,
not a cache of logical page results. The optional flat-directory allocation is
released with the lease; retained scratch contains only two bounded groups
and bookkeeping. Contexts must remain on their transaction's worker thread.

Native Map, List and String contexts also reuse their already-bound transaction
root for page lookups and staging. Root keys and entry addresses remain stable
for the callback. This internal path is private to these contexts; generic
owner-based APIs still validate every address. Both paths share lookup and
staging implementations, observe live erase/clear flags and pending pages,
and invoke the existing physical backend hooks. No page results are cached.



AVL height and subtree count allow deterministic logarithmic traversal instead
of relying on a collision-prone hash or a root containing all bucket addresses.
One node per field has memory overhead compared with packed small-field pages;
this is not a throughput claim or an optimal packing result. Sorted insertion,
random deletion and tree rotations preserve the balance/count invariants.
Double rotations permit the intermediate imbalance required by their first
step, then require a balanced final root.

## Operations and ownership

The context supports get, field-value length, insert/update/NX, erase, root-only
expiry, whole-owner clear, bucket iteration and independent cloning. It must finish inside its existing logical-key
transaction. Ignored errors poison that transaction. An unfinished mutation
aborts on context destruction. Initialization is an explicit owner replacement;
callers must authorize it after type/expiry checks, just as for List storage.
The storage component does not interpret Redis arguments or migrate legacy
payloads itself; the command DAO supplies that policy.

Point lookup compares stored key fragments against the requested field without
building a complete stored field buffer. Mismatching comparisons stop after
the relevant prefix. Value-length lookup never loads value fragments. GET
assembles only the requested return value. Replacing a value leaves its key and
unrelated fields intact. It erases old value fragments and creates bounded new
ones rather than encoding the complete map.

Insert/delete update only the search path and rotated nodes. Two-child deletion
transfers the successor's descriptors into the surviving node, without copying
its large field/value payloads. Removal reclaims both descriptors' old fragments;
the last field or explicit clear uses whole-family reclamation. Native freeing
remains synchronous and proportional to reclaimed pages, even though clear's
staged event is bounded. Existing page transaction and observer group limits
still apply.

Visited nodes validate ids, high-water bounds, reserved bytes, height, subtree
count and local balance. Mutation checks the root's declared count against its
node. Fragment reads validate id, offset, progress, remaining length and end
links. The hard height bound limits malformed tree recursion. These checks do
not audit untouched branches, detect arbitrary cross-field aliases, provide
checksums, or supply durable corruption recovery. A full audit is used in tests,
not on every point operation.

## Iteration and cloning

A cursor retains only the AVL ancestor path, with a hard height bound. It seeks
the first requested bucket and walks successors, avoiding subtrees belonging to
earlier buckets. Visitors may stop early and may omit value payloads. Returned
fields/values are assembled only as requested by that visitor or scan response.
The visitor must not mutate the map; an error can follow an already visited
prefix, so callers assembling a complete response must discard that prefix on
failure.

SCAN uses bucket numbers as opaque continuation cursors, independent of ranks
and rotations. A page finishes a whole bucket after reaching its COUNT hint.
Deleting other fields cannot shift stable fields behind the cursor. MATCH
rejections and NOVALUES do not load value fragments. COUNT is not a hard result
or I/O bound: a crowded bucket may return many fields. Out-of-range cursors end
cleanly. Scan output is published only after a page succeeds, retaining the
caller's previous output on failure.

Cloning validates the reachable tree in order, its heights/counts, stable bucket
hashes, strict field order and unique ownership of node/blob ids. It streams
fragments into a separately reset destination family, retaining ids and exact
expiry. Adjacent large field names in the same bucket are compared with two
bounded readers, never complete field buffers. A scalar id set detects aliases;
its size grows with reachable pages, so this is not a constant-resident-memory
claim. No full Hash or complete large field/value buffer is required by clone.
Unreachable source orphans are not enumerated or copied.

The source remains read-only. `db::copy_pages_to` supplies the existing ordered
same/cross-DB key locks; the destination's root/pages commit together, including
reclamation of its previous family. Native errors, malformed sources, ignored
failures and unfinished clone contexts prevent destination publication. This
is also used by Hash COPY/RENAME after their option/type/expiry checks.

## Evidence and remaining work

`bounded_map_store` independently audits tree ordering, balance, counts,
fragment ownership and physical-page reachability against a `std::map` model.
It includes:

- 1600 deterministic model operations with empty/binary fields and large values.
- 10000 ordered fields; one inline-field update writes one node and accesses
  fewer than 70 native pages, without walking or serializing the full map.
- A 40000-byte field and 1 MiB value, with every physical value at most 16 KiB.
- Byte-truncation codec checks and malformed/oversized record rejection.
- Allocation-failure sweeps for large-value replacement and deletion; failures
  preserve original roots, field values and page ownership.
- Four threads each running four fibers, yielding between put/read/erase inside
  the same transaction and retaining the original field after completion.
- Both deletion orders through an empty map, Set-kind values, ignored errors,
  wrong kinds, broken counts/heights/links, missing pages, bad fragment offsets,
  future fragment versions and exhausted counters.

The native implementation runs on `mdb`, `mdb_tbb`, single-instance `rdb`
and `wdb`. Disk transaction limits are in `ROCKSDB_STORAGE.md` and
`WIREDTIGER_STORAGE.md`.
Other engines still need native page support; Hash/List requests explicitly fail there instead of
writing whole serialized collections. Set commands reuse this membership storage. ZSet coordinates member lookup
and score traversal in one page transaction. Full synchronization, replay, durability
and performance comparisons remain open requirements of the Redis server goal.


`bounded_map_iteration` checks full/early visitors, 1202-field scan coverage,
300 stable fields under per-page insert/delete churn, NOVALUES/MATCH payload
avoidance and a small scan on 10000 fields reading fewer than 500 native pages.
It clones two same-bucket fields over 1 MiB, including a 1 MiB value, with no
individual C++ allocation of 32 KiB or larger in that fixture. Other fixtures
check independent same/cross-DB ownership, old-family reclamation, Set-kind
clones, allocation rollback, DB 7 page publication/application, alias/bucket/
future-fragment rejection and failed-scan output retention. Existing point and
allocation tests run against the revised bucket layout too.


## Hash command integration

`dao::paged_hash` implements the existing HSET/HMSET/HSETNX, HGET/HMGET,
HGETALL/HKEYS/HVALS, HDEL, HLEN/HSTRLEN/HEXISTS and HINCRBY routes. Each mutation
and its results commit under one logical-key lock. Lookup/type/integer and
overflow behavior retain the existing tests. HLEN reads root metadata only;
HKEYS skips value fragments. HGETALL and HVALS assemble the requested response.

On supported memory backends, small Hashes use a PMR v7 root without child
pages. It holds up to 63 fields and 2048 payload bytes, including one length
byte per field. Writes that exceed either limit promote to the paged index.
Reading raw roots beyond the previous 32-field or 512-byte limit requires
a PKV build that supports the larger limits.

Native metadata views expose Hash/Set type, count and absolute expiry and reject
opaque payload transfers. TYPE/EXISTS/SCAN TYPE, TTL/PERSIST, DEL and String
replacement recognize PMR roots. Metadata-only expiry updates preserve member
pages and publish root-only events. COPY/RENAME choose List or Map cloning from
the locked source kind, including across DBs; target decoding cannot overwrite
that choice. Replacing another type resets its old family atomically.

The first non-scan Hash DAO access to a legacy JSON Hash streams fields through
`record_view::hash_visit`, preserving its absolute TTL. The JSON implementation
keeps its original parser document and one field/value pair at a time, without
a second complete field map. Duplicate names and invalid field values abort the
migration together with the requested command. Custom codecs retain a default
visitor fallback. Legacy empty Hash records normalize to key absence. Legacy
HSCAN views retain their existing snapshot path; subsequent native scans reopen
the current root and traverse pages under the logical-key lock. Wrong types and
failed scans do not reuse prior views or partial results.

PKV2 groups carry Hash root/node/blob changes. V1 subscribers that need those
events disconnect; DB-0-only legacy peers ignore nonzero DB groups. Native
transactions still obey the 64 MiB/65536-frame group limits before commit, even
though each individual page is bounded. This is not yet full Redis size,
replication, field-expiry or command coverage.

`paged_hash_dao` compares 600 commands/results/errors against the legacy DAO,
checks bounded roots, root-only HLEN/TTL, HKEYS/HSCAN NOVALUES payload avoidance,
1 MiB legacy values, failed-command and malformed-record migration rollback,
cross-DB COPY over another type, independent ownership, exact TTL, RENAME,
PERSIST, immediate expiry and allocation-failure retention. Existing client,
scan-churn, shared-counter and cluster tests now exercise the native DAO. Real
subscriber tests inspect actual Hash pages and root-only expiry; a source/replica
pair verifies binary fields, large values, cross-DB copy, mutation, rename and
persistence removal.

## oneTBB integration

`mdb_tbb` now supplies atomic root/page batches, binary-safe key lookup and
stable bucket SCAN. Hash, List and Set commands use the same bounded native formats
on both memory backends. GET uses a short-lived `const_accessor`; no oneTBB
accessor remains held across a fiber yield. See [TBB_STORAGE.md](TBB_STORAGE.md)
for rollback, locking, validation and benchmark scope.

## Embedded native indexes

The ZSet storage component embeds two Map AVL trees in one owner family with a
shared monotonic page-ID allocator. Embedded contexts publish tree state into
the parent rather than writing separate PMR roots. The score tree uses lexical
ordering with bucket zero; the member tree retains stable hash buckets. Parent
reset clears both indexes once. Embedded clone through the standalone Map API
is rejected. The physical blob envelope allows an extra eight-byte score prefix;
ordinary Hash/Set keys and values retain the 512 MiB logical limit. See
[ZSET_STORAGE.md](ZSET_STORAGE.md) for the new component and integration gaps.


HRANDFIELD uses subtree ranks to select fields without materializing the whole
Hash. WITHVALUES reads the selected node's value in that same tree lookup;
field-only samples skip value fragments. Hash/Set/ZSet share a sparse rank
sampler. See [Hash sampling](HASH_SAMPLING.md) for semantics, complexity and
validation scope.


HINCRBYFLOAT now uses `get_bounded` to reject fields exceeding the numeric
parser's size bound before reading value fragments, while retaining one lookup
for a valid field. It edits only the selected field and normal AVL ancestors;
key TTL and atomic legacy migration remain unchanged. See
[floating increments](FLOAT_INCREMENTS.md).
