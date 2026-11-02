# Logical databases and COPY

SELECT now changes only the issuing connection's logical database. New
connections start at DB 0. The `databases` configuration defaults to 16 and may
be set from 1 through 65536; `db_count` remains the physical memory shard count
within each DB. Each logical DB owns an independent backend instance, keyspace,
TTL state, locks and scans. Every existing command family uses the connection's
selected shared DB handle, including commands within a single pipeline.

A shared database pool lazily creates DB N on first use under a fiber-aware
mutex. Creation is serialized, only a successful instance is published, and
failed creation can be retried. SELECT leaves the previous selection intact on
failure. Connection handles keep the pool and their selected DB alive. Startup
still opens DB 0 through the original backend/path layout; no existing keys are
prefixed, moved or re-encoded. For each configured storage root, DB N adds
`/logical-db-N` before the backend's usual rdb/wdb subdirectory. Multi-root paths
retain the same partitioning within each DB. Memory backends ignore these paths.
Disk backends and their reopen/durability behavior are not validated here.

Cluster mode permits SELECT 0 and rejects other indexes. COPY participates in
ordinary two-key slot routing and rejects a nonzero destination DB once command
execution is reached. Current routing tests use unassigned cluster slots; topology
migration/failover remains unverified. Cursor tokens belong to their logical
DB and lifecycle, so an unfinished SCAN must not be continued after SELECT to a
different DB or after changing storage instances.

## COPY semantics and locking

COPY accepts source, destination, optional DB index and REPLACE. DB defaults to
the currently selected DB; it never changes that selection. Repeated DB options
use the last valid index, and repeated REPLACE is accepted. All DB/index options
are validated before lookup in noncluster execution. Missing/expired sources
return 0. A live destination returns 0 unless REPLACE was supplied; an expired
one does not block copying. A source/destination with equal key names is valid
across distinct DBs, but the same object in one DB returns an error even if
missing. Source bytes/type/TTL remain unchanged, and the destination receives an
independent copy with the source's absolute expiry. Replacing from a persistent
source removes the destination's previous expiry.

Same-DB copies reuse the common sorted/deduplicated key transaction. Cross-DB
copies lock the two DB identities in `std::less<db*>` order, then their source
and destination key stripes, before reading either record. This order is the
same for opposite-direction copies. Other mutators/replication apply use those
same stripes. The callback may propose only a destination change and must not
recursively access either DB. Exactly one backend write is needed; failed lookup,
encoding or write leaves both stored values unchanged and clears the result flag.
This is local command synchronization, not a cross-engine durable transaction.

Source payload validation uses the optional codec validation boundary introduced
for rename. Equal immutable codec instances copy exact encoded bytes without
encoding, preserving legacy versions and allowing read-only codecs. Different
codecs materialize the logical source and encode with the destination codec;
unsupported conversion/precision fails before commit. Corrupt/future sources are
preserved. Without REPLACE, an undecodable destination reports an error; REPLACE
overwrites its old bytes without decoding them. Whole-record decode, validation,
copy/transcoding and source/target lock duration still depend on record size.

## Replication, persistence and remaining work

The custom live stream now negotiates a version 1 header with explicit DB identity.
Nonzero DB dispatchers publish DB-scoped updates, and upgraded receivers apply
those records to their configured pool without echoing observers. Legacy
subscribers still receive DB 0 only. Optional replica_rpc_source configures a
live receiver; it does not copy initial data or replay missed updates after a
disconnect. See [replication protocol and limits](REPLICATION.md). Do not infer
full multi-DB recovery/failover support from these live-update tests. Cluster
mode retains its DB 0 restriction.

[Public MOVE](MOVE.md) now has native coordination and ordered publication;
DBSIZE is also available. SWAPDB, database flush commands, Redis persistence
interoperability, full replication recovery, resource limits for many DBs and
production performance remain unfinished.
Additional DBs are created lazily, but each active DB still allocates the
configured physical shard structures. No claim of low fixed cost for thousands
of simultaneously active DBs is made.

## Verification

The ordinary suite adds 1,341 assertions per endpoint: connection-local SELECT,
invalid indexes and unchanged selection, pipelines, all five value types,
independent scans, same-name cross-DB copy, source/target expirations, REPLACE,
repeated options, missing sources and six clients doing opposite-direction copies.
A separate temporary service configured with `databases=3` adds nine checks per
endpoint. Default configuration remains covered by the ordinary fixture.
Eight cluster additions bring routing coverage to 338 assertions per endpoint.

Storage tests check one-time concurrent pool creation, failed creation/retry,
lookup and destination-write failures, encode failure, identical-object rejection
and untouched sources. Codec tests convert text-to-JSON and back with exact
millisecond expiry, and copy through a read-only selective codec without invoking
its encoder. C++17 ASan/UBSan and C++11 Release run all twenty-four CTest entries on the tested Unix host
against the local Redis reference build. These are bounded local memory tests.


The subsequent [prepared page commit foundation](PREPARED_PAGE_COMMITS.md)
adds reversible two-memory-engine preparation before either publication, while
preserving COPY's existing one-target commit. The later [public MOVE](MOVE.md)
adapter connects paired effects, native journals and PKV4 capture. Durable
disk mixed-group replay remains a separate requirement.
