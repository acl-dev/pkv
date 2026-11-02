# Native Set storage

Redis Set commands use `dao::paged_set` on `mdb`, `mdb_tbb`, single-instance
`rdb` and `wdb`. Set members
are keys in the bounded Map AVL index with `map_kind::set` and empty values.
The 48-byte PMR v2 root stores count, TTL and page ownership metadata. Each PMN
node is independently stored; large members use PMB fragments at most 16 KiB.
No Set command serializes the whole collection back into one stored value.
See [MAP_STORAGE.md](MAP_STORAGE.md) for the shared binary layout.

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

The final full matrix passed: C++11 Release and C++17 ASan/UBSan, each with
oneTBB disabled (28/28 CTests) and enabled (29/29). The enabled builds run the
five process-level fixtures against `mdb_tbb`; other native DAO unit fixtures
use `mdb`. This is compatibility/rollback evidence, not proof of race freedom,
production durability or throughput superiority over Redis.
