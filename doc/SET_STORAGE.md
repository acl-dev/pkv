# Native Set storage

Redis Set commands use `dao::paged_set` on `mdb`, `mdb_tbb`, single-instance
`rdb` and `wdb`. Set members
are keys in the bounded Map AVL index with `map_kind::set` and empty values.
The PMR root stores count, TTL and page ownership metadata. On insertion of
the 257th member, a Set promotes to a grouped directory under a PMR v5
or wide PMR v3 root.
Point operations then search one of 256 AVL buckets. Each PMN node is stored
independently, and large members use PMB fragments at most 16 KiB.
See [MAP_STORAGE.md](MAP_STORAGE.md) for the shared binary layout.

On supported memory backends, small Sets use packed roots without child pages.
PMR v6 retains its bounds of 63 members and 1024 payload bytes. PMR v8 extends
these bounds to 128 members and 4096 payload bytes; each member is at most 255
bytes and contributes one length byte to the payload. Writes exceeding these
bounds promote to the paged index. Values within the v6 bounds still encode as
v6; edits to existing v8 values may retain v8 after shrinking.

On these memory backends, SREM, SPOP and a successful SMOVE may repack a
reduced native Set into a compact root when at most 128 members remain and
the member/payload limits fit. Finalization validates the entire surviving
graph, member order, unique node/fragment ownership and TTL before atomically
clearing its child pages. Allocation or validation failure aborts the command.
Oversized members or payloads retain their paged layout; read-only operations
do not repack. This includes already-small unpartitioned native Sets.

Both versions have the four-byte PMR/version prefix and a flag byte at offset 4.
In v6 the low six flag bits contain the count. In v8 those bits must be zero,
and an independent byte at offset 5 contains a count from 1 through 128.
Bit 6 selects two-byte rather than one-byte offsets; bit 7 signals an eight-byte
big-endian deadline immediately after the header. Offsets follow the optional
deadline, then sorted, unique, length-prefixed members. One-byte offsets require
at most 256 payload bytes. Decoders enforce each version's own limits.

A per-thread v8 validation cache stores owned bytes and scalar metadata. Every
reuse requires complete byte equality; address or sampled-byte equality alone
never bypasses validation. Repeated samples only control cache admission after
successful validation, and cache allocation failure falls back to normal reads.
TTL checks remain live on each access.

Upgrade readers and replication receivers before producing v8 roots. Older
builds cannot read v8; mixed-version replication has not been validated.
Reading v6 roots beyond the earlier 32-member or 512-byte bounds also requires
a build supporting the larger v6 limits.

## Commands and atomicity

SADD/SREM edit member nodes and balance ancestors. SCARD reads the root only;
SISMEMBER/SMISMEMBER search the index without reading unrelated member blobs.
SMEMBERS streams the index into the required owning response. Empty Sets remove
the key and its page family. SSCAN uses stable scan buckets, supports MATCH and
COUNT, and never flattens a native Set into JSON.

SMOVE locks both logical keys in the standard total order. It validates a live
source's destination type even when the member is absent, preserves both TTLs
and commits both edits together. A missing source skips destination validation.
An identical source/destination returns membership without deleting the key.

SUNION/SINTER/SDIFF and STORE variants validate every distinct input first.
Intersection iterates the smallest input; difference iterates its first input;
other inputs are probed by indexed membership. Repeated inputs retain their
Redis semantics, including SDIFF of a key with itself. Results own response
strings, but inputs are not all copied into whole-collection snapshots. STORE
writes bounded pages, accepts any old destination type and clears its TTL.
A destination that aliases an input is replaced only after reading all inputs.
SINTERCARD uses the smallest input and stops after LIMIT accepted members,
without constructing an intersection result.

SRANDMEMBER and SPOP select zero-based member ranks using subtree counts.
Grouped Sets use directory counts to locate the bucket containing a rank.
Small unique samples use sparse Fisher-Yates over numeric ranks with O(k)
scratch space, independent of the full collection size. Negative SRANDMEMBER
counts select ranks with replacement. Requesting every member streams the index.
SPOP selects before erasing and commits removals atomically; failures clear the
partial output. Randomness remains non-cryptographic and per worker.

All native edits use page transactions. Allocation, decoding or destination
errors abort roots and pages together. Shared key operations handle TYPE, TTL,
COPY across logical DBs and RENAME using the same bounded Map ownership model.
Clones have independent pages and preserve exact TTLs; STORE results are persistent.

## Compatibility and migration

The decoder reads the existing unpartitioned Set layouts. A Set that
already exceeds the promotion threshold can continue using its original
tree. Upgrade replication receivers before sending grouped Set records.

Old JSON Sets migrate on first Set DAO access, including SCARD. Migration reads
one validated legacy view and inserts members into independent native pages,
keeping the old TTL. It does not construct a second full member container.
The existing JSON decoder still constructs a membership index during legacy
decode; migration peak memory is not constant. A rejected command aborts its
migration, leaving old bytes and page ownership unchanged. Legacy SSCAN may
continue using a JSON snapshot until another Set DAO operation migrates it.

ZUNION/ZINTER/ZDIFF, STORE variants and ZINTERCARD accept native Sets as inputs
with member score 1. The default native ZSet DAO reads Set/ZSet sources under
one page transaction and writes bounded ZSet destination pages, including when
the destination aliases a Set source. The old `transact_page_reads` bridge is
retained for legacy DAO fixtures; current server ZSet results no longer use
whole JSON roots.

PKV2 sends native roots/pages in atomic groups, including multi-key SMOVE and
STORE edits. TTL-only changes send just the root. V1 peers cannot represent the
pages and are disconnected instead of receiving an incomplete value; DB-0-only
legacy peers skip nonzero-DB events as before. Initial sync/replay is still open.
Engines without page transactions explicitly reject Set requests rather than
fall back to whole-value writes.

## Validation

`paged_set_dao` compares 600 deterministic operations with the independent
legacy DAO, including duplicate inputs, destination aliases, binary/empty and
40000-byte members. It checks bounded roots, root-only SCARD, rank selection
against complete traversal, small-sample locality on 10000 members, unique and
repeated sampling, pop removal, zero/all/missing samples and signed limits.
Allocation sweeps cover SMOVE, aliased-input STORE and SPOP rollback.
Migration/copy tests include a 1 MiB member, exact TTL, complete paginated SSCAN,
independent cross-DB COPY/RENAME, rejected migration and malformed/future records.
Mixed Set/ZSet tests verify source-alias replacement, results larger than the
native root limit, and reclamation of the old native family.

Socket compatibility exercises all existing Set, sampling/cardinality, SCAN,
mixed Set/ZSet algebra and cluster cases through the new default DAO. Replication
fixtures inspect bounded Set subscriber frames, root-only TTL, v1 rejection and
actual source/receiver values after SMOVE, UNIONSTORE, COPY, RENAME and PERSIST.

The initial native Set matrix passed: C++11 Release and C++17 ASan/UBSan, each with
oneTBB disabled (28/28 CTests) and enabled (29/29). The enabled builds run the
five process-level fixtures against `mdb_tbb`; other native DAO unit fixtures
use `mdb`. This is compatibility/rollback evidence, not proof of race freedom,
production durability or throughput superiority over Redis.

The 2026-11-07 v8 extension passed the current full Release suites (132 CTests
with oneTBB disabled, 146 enabled). Its added DAO cases cover the 63/64/65/128
boundaries, 256-byte narrow and 4096-byte wide payloads, high deletion-mask bits,
TTL edits, malformed headers, optional-cache OOM and rollback on promotion.
An independent C++11 ASan/UBSan codec probe checks legacy byte equivalence and
wide-value mutations; it is not a full-project sanitizer run. Four-worker public
protocol probes passed 414 checks on each memory backend. Performance samples
and the remaining regressions are recorded in the
[v8 benchmark report](benchmarks/performance-wide-compact-set-2026-11-07.md).

The subsequent short-member validation optimization preserves that layout and
all structural checks. It compares 8–32 byte common prefixes as unsigned
big-endian words with bounded overlapping tails, retaining generic comparison
for other lengths. Guard-page and C++11 ASan/UBSan probes cover binary ordering
and 40000 decoder mutations against the previous decoder, including a forced
generic byte-order path. The
[validation performance report](benchmarks/performance-set-validation-2026-11-07.md)
includes P1/P64 results, memory controls and the remaining negative samples.
