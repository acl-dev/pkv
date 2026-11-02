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
| Root | PMR v2 | 48 bytes | Kind, expiry, count, tree id, highest id, reserved |
| AVL node | PMN v2 | 80–272 bytes | Id, bucket, children, height/count, two blob descriptors, inline data |
| Blob fragment | PMB v1 | 33–16384 bytes | Id, next id, byte offset, nonempty payload |

The root kind is Hash or Set. Exact millisecond expiry is stored as a signed
64-bit deadline, with `-1` for persistence. Count is bounded by signed 64-bit
range. Empty maps remove their root and native family. The monotonic id counter
is shared by tree nodes and blob fragments; exhaustion fails without wrapping.
Other page versions return an unsupported-format status; reserved bytes must
remain zero.

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
Hash commands now create version 2 directly.

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
HKEYS skips value fragments. HGETALL and HVALS naturally assemble their requested
response, but no command stores a serialized copy of the complete Hash.

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
