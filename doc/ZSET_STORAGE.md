# Native ZSet storage component

`db/zset_store` implements two AVL indexes in one native page family. Existing
Redis ZSet commands route through `dao::paged_zset` on `mdb`, `mdb_tbb` and
single-instance `rdb` and `wdb`.
Old JSON ZSets migrate atomically on first ZSet DAO access. Generic TYPE, TTL,
COPY/RENAME and ZSCAN understand the new roots, and PKV2 replicates root/page
transactions. Engines without page transactions reject these commands explicitly.

## Layout

The PZR v1 root is 64 bytes, using big-endian unsigned fields:

| Offset | Bytes | Meaning |
|---:|---:|---|
| 0 | 4 | PZR and version 1 |
| 4 | 4 | Reserved zero bytes |
| 8 | 8 | Exact expiry milliseconds; all ones means persistent |
| 16 | 8 | Number of logical members |
| 24 | 8 | Member-index root page ID |
| 32 | 8 | Score-index root page ID |
| 40 | 8 | Shared highest allocated page ID |
| 48 | 16 | Reserved zero bytes |

Nonempty roots require two distinct nonzero root IDs and enough page IDs for
at least two nodes per member. Empty ZSets delete the root and page family.
Unknown versions return unsupported-format errors; malformed lengths, reserved
bytes, TTLs, counts and root pointers fail validation.

Both indexes reuse bounded PMN v2 AVL nodes and PMB v1 fragments from Map storage.
No physical value exceeds 16 KiB. The member tree orders by stable scan bucket
and member bytes, storing an eight-byte score. The score tree orders by lexical
`encoded score || member bytes`, with an empty value. Equal scores therefore
sort by binary member bytes. Members may contain empty/arbitrary binary strings;
large members are independently fragmented in both indexes.

Sortable scores use an IEEE-754 double transform: invert all bits for negative
numbers and flip the sign bit for nonnegative numbers, then write big-endian.
This preserves numeric order under byte comparison, including infinities and
subnormal numbers. Sorting keys use positive-zero encoding for both zeros and
reject encoded negative zero. Member score payloads use the same transform but
preserve negative zero, retaining `-0` replies for ZSCORE/range/scan/pop. NaN is
rejected in both forms. Existing canonical payloads remain readable. The internal
score-key blob limit allows eight extra prefix bytes while the logical member/value limit stays 512 MiB.

## Shared tree context

`map_store` now has an internal embedded-tree constructor. Each tree retains its
own root/count but shares the parent's page-ID allocator and logical owner.
Embedded finish publishes only local tree metadata; the parent writes PZR after
both trees finish. Embedded operations never reset the shared page family on
first insertion or final removal. Parent initialization/clear resets the family
once, then resets both trees. Ordinary Hash/Set contexts keep their existing
standalone root behavior. Standalone clone rejects embedded contexts so it
cannot accidentally copy just one index or reset the other tree's pages.

Put validates an existing member's score-index entry, removes its old score key,
updates its member entry and inserts the new score key. Erase removes both index
entries. Counts must agree before finishing. All changes are staged in the same
`page_transaction`; failed allocations or abandoned contexts abort both trees.
A no-op score update leaves both trees untouched. TTL-only edits write the PZR
root without reading or rewriting member pages.

Member lookup and rank/select use indexed searches and subtree counts. Rank
looks up the member score and then searches the score tree. Selection verifies
that the selected score key agrees with the member index. Member and score-order
visitors may stop early. Member-bucket scan converts binary scores to Redis score
text. Operations do not serialize or materialize the complete ZSet as a value;
responding with an individual member still requires its owning string.

## Validation and remaining integration

`bounded_zset_store` tests numeric/byte ordering of more than 10000 score values,
root truncation and version checks, deterministic model edits, index order and
AVL metadata, disjoint page ownership, bounded fragments and orphan reclamation.
Fault sweeps cover score replacement, erase and clear/reseed. Rank/select,
member-bucket scan, infinity/zero handling, early-stop visitors and 1 MiB members
have direct coverage. A 10000-member fixture checks point-update locality and
root-only expiry changes. Four threads each run four fibers with yields inside
atomic two-key updates. In a oneTBB build the model/fault/concurrency fixtures
also execute the oneTBB backend.

The component supplies coordinated clone/transfer and range seeks/counts.
The native DAO, migration, key/scan integration and page replication are now
active. Disk page transactions, initial replica synchronization/replay and
broader Redis services remain required.
Full Redis compatibility, disk pages, replication initial sync/replay and
production throughput remain open requirements.

Final full regression passed on C++11 Release and C++17 ASan/UBSan, each with
oneTBB disabled (28/28 CTests) and enabled (29/29). Existing command/replication
fixtures remain green after the shared Map changes. Native ZSet tests exercise
both memory engines in the enabled builds. These checks do not establish general race freedom, durability or Redis
throughput superiority.

## Range seeks and counts

`map_store::bound_rank` finds lower/upper bound ranks through subtree counts.
Prefix bounds on lexical trees compare only the required key prefix, reading
bounded fragments. ZSet score endpoints use eight-byte score prefixes, so
inclusive/exclusive bounds cover all equal-score members, including an empty
member. Rank and score ranges apply reverse order and LIMIT/OFFSET by arithmetic
on rank intervals. `visit_rank` seeks directly to the first requested rank and
then walks the requested interval with an ancestor stack, skipping preceding
members. Score count uses two bounds without materializing matched members.

For BYLEX, the component first checks whether every score is equal using one
member score and score-prefix ranks. Uniform-score lex ranges/counts then use
full score/member bounds and the same rank cursor. Mixed-score lex queries keep
the existing JSON DAO's member-byte semantics, which require scanning the
bucketed member index. Count does not retain members. A finite result keeps only
its top `offset + limit` candidates; unlimited output retains the requested
response strings. Mixed-score lex access is therefore not an indexed logarithmic
operation, and offset-heavy queries can still retain many candidates.

## Independent two-index cloning

`zset_store::clone_to` resets the destination family once and copies both trees
into it with the source's page IDs and shared allocation sequence. Map clone
helpers validate tree metadata, strict ordering and fragmented keys with bounded
page buffers. The scalar visited-ID set is shared between both traversals,
rejecting node/blob aliases across indexes. Parent finish writes one PZR root.
The ordinary standalone Map clone API still rejects embedded contexts.

After physical traversal, cloning checks every member's valid score payload and
matching empty score-index entry. This rejects logically inconsistent indexes
before commit. Verification owns one member at a time and can allocate a large
individual member; it does not materialize the whole collection. Cloning is not
constant-memory or linear-time verified: visited IDs scale with page count and
cross-index probes add logarithmic work per member. Failure rolls back the old
destination while leaving the source unchanged. Same-owner self-clone aborts;
a missing source leaves the destination untouched. Same-DB staged source edits
are visible to cloning, while cross-DB copy uses the existing read-only source
transaction.

`native_zset_ranges` compares 1200 queries for each of uniform/mixed score sets
against the existing JSON view, including binary/large members, reversed ranges,
exclusive bounds, infinities and pagination. Explicit signed-extreme/NaN cases,
1 MiB independent same/cross-DB copies, TTL retention, arbitrary destination
replacement, staged edits, inconsistent-index rejection and allocation-failure
sweeps supplement the models. In oneTBB builds these fixtures also run against
oneTBB. A 10000-member fixture checks score count, distant rank seeks, large
OFFSET with a small result and uniform-score lex count without full traversal.
These storage-component tests are supplemented by native DAO and real-server
command/replication coverage below.

## Default Redis command path

`paged_zset` implements ZADD options/increment, ZREM, ZCARD, ZSCORE/ZMSCORE,
ZRANK/ZREVRANK, range/count/removal aliases, ZRANGESTORE, ZPOPMIN/MAX, ZMPOP,
ZRANDMEMBER and ZUNION/INTER/DIFF with STORE/cardinality variants. Each native
write commits one bounded PZR root and touched pages. Full-collection removals
clear the owner family after preparing the required response instead of deleting
every node separately. Native ZCARD reads only metadata. Small random samples
use sparse rank shuffling rather than copying/indexing the full population.

Weighted algebra accepts legacy/native Sets and ZSets, validates source types
before options, preserves repeated weighted contributions and cardinality-based
input order, and short-circuits cardinality LIMIT without retaining a result.
Other algebra results use an owning result map, then bounded destination pages;
inputs are not all copied into collection snapshots. STORE accepts any old target
type, clears TTL and handles source aliases after all reads. Result memory is
not generally bounded independently of output size.

First-access migration reads the validated old JSON view and inserts members
into both trees with exact TTL. It retains that legacy view until the command
commits; peak migration memory is not constant. An invalid increment, malformed
record or failed commit leaves old bytes/pages unchanged. Native page versions
remain guarded. Expired native roots rebuilt by a new ZADD reset the old page
family before allocating IDs, preventing unreachable old member fragments.

Generic COPY/RENAME clone both indexes and preserve TTL; PERSIST/expiry modify
only the 64-byte root. ZSCAN uses stable member buckets and emits decimal scores,
including signed zero. PKV2 transports bounded ZSet roots/pages atomically. V1
peers disconnect on native page events rather than receiving incomplete values;
legacy DB-0 peers continue skipping nonzero DB events. Initial sync/replay is
still not implemented.

`paged_zset_dao` compares 600 operations with the legacy DAO per engine and
inspects native roots. Migration fixtures include a 1 MiB member, infinity,
negative zero, failed-increment rollback and exact TTL. It exercises generic
cross-DB COPY/RENAME/PERSIST and full paginated ZSCAN, rejected malformed/future
records, expired-owner page reclamation, sampling and allocation sweeps for
ZADD, aliased algebra STORE, ZRANGESTORE and ZMPOP. Existing socket/cluster and
concurrent ZSet fixtures now execute the native DAO. Subscriber tests inspect
PZR/member frames and root-only TTL; a real source/receiver pair checks large
members, signed zero, score changes, UNIONSTORE, RANGESTORE, pop, COPY and RENAME.

The final native-server matrix passed: C++11 Release and C++17 ASan/UBSan,
oneTBB off (28/28 each) and on (29/29 each). Enabled process fixtures run the
oneTBB backend; native DAO fixtures additionally exercise both memory engines.
No replica initial sync/replay, general race freedom,
full Redis completeness or production throughput superiority is claimed.

RocksDB per-instance native root/page/index batches and reopen/process-kill
recovery are now available. Cross-path write limits and normal WAL sync policy
are described in [ROCKSDB_STORAGE.md](ROCKSDB_STORAGE.md). WiredTiger now supplies
per-connection snapshot root/page/index transactions, binary storage and recovery
fixtures; see [WIREDTIGER_STORAGE.md](WIREDTIGER_STORAGE.md).
