# Stored root counts

DBSIZE returns the number of stored user roots in the selected logical DB.
Native payload pages, scan indexes, backend metadata and physical shard counts
are excluded. It is node-local in cluster mode and has no key arguments. It
does not parse values, remove expired roots, migrate old encodings, modify TTL
or invalidate WATCH. A lazy-expired root remains counted until an operation
physically removes it. KEYS filters expiry and may therefore return fewer keys.
Primary [active expiration](ACTIVE_EXPIRY.md) now performs this cleanup; it can
be disabled or deferred, so counts still describe stored roots. The response follows the
[Redis DBSIZE contract](https://redis.io/docs/latest/commands/dbsize/).

## Query and mutation paths

`db::key_count` takes exclusive execution admission for one coherent total.
Handlers select that mode before acquiring ordinary shared command admission.
The query sums physical instances; its work is independent of stored key count
at fixed topology. Admission may wait for concurrent commands to finish.

- mdb keeps a root counter per existing physical shard, under its shard lock.
  A successful new insertion increments it; deletion decrements only if a root
  was actually removed. Batch insertion counters publish only after all
  allocating preparations have succeeded, so rollback cannot leave a prefix.
- oneTBB uses its existing owner-map sizes. It adds no second per-key counting
  index or counter update to the write path. This does not make the complete
  execution path lock-free: logical command locks and admission still apply.
- RocksDB and WiredTiger keep an atomic count per physical instance. Successful
  engine commits add their net root delta before releasing logical key locks
  and command admission. Overwrites and page-only updates do not change it.
  No new persisted counter, write hotspot or disk I/O is added to normal commits.
- A source workspace keeps its own net delta as root states publish. DBSIZE
  combines that scalar with the frozen source total, without scanning touched
  owners. Delete/recreate, repeated overwrites, discarded work and failed commit
  retries preserve the expected count. Only a successful final commit changes
  the source engine count.

Legacy/raw disk SET now checks actual existence before declaring an insertion;
otherwise repeated replication or direct raw overwrites could overcount.
Raw disk DEL also checks existence and returns false for an absent key. Native
transaction paths already have authoritative found flags and need no additional
existence lookup. Legacy replica SET/DEL, native replication, key transfers and
source workspace commits use the same backend count publication paths.

## Startup and durability

Each RocksDB/WiredTiger instance counts its actual root records when opened,
after the existing schema/index bootstrap or legacy import. This takes O(N)
startup work and reads root keys without decoding values or loading payload
pages. It adds latency for large cold stores; it is not a constant-time open.
After startup, DBSIZE needs no iterator, root read, page read or engine session.

Counts are derived state, not separately durable metadata. An independent
process reconstructs the total from the roots actually recovered by its engine,
so a crash cannot leave a durable count ahead of the recovered database. This
does not strengthen existing engine durability: RocksDB retains its prior
non-fsync normal write policy and WiredTiger retains synchronous logging.
No storage format or replication wire version changes. Engines without a count
implementation return an explicit unsupported error. A count outside the RESP2
signed integer range or a failed engine count produces a storage error rather
than a wrapped reply.

## Verification

The storage fixture checks raw overwrite/delete, duplicate replica operations,
large paged String values, TTL preservation, expired-root retention/removal,
workspace histories, discard and rejected-commit retry. Memory OOM sweeps cover
89 budget positions including the successful control; oneTBB repeats the cases
on both memory engines. A query spy rejects root reads/scans, and a zero C++
allocation budget verifies the mdb query's direct count path. Concurrent workers
create and overwrite disjoint keys while a reader requests coherent totals.

Native engine variants run the same root-count checks, concurrent root changes,
independent disk reopening and cross-physical-store aggregate/rejection cases.
The socket fixture runs the same expected replies against an isolated local
Redis reference and PKV: all five types with 70-KiB payloads, binary/empty keys,
COPY/RENAME, logical databases, WATCH, staged EXEC and DISCARD. Separate cluster
checks cover unserved-slot counts and queued counts. The reference is a local
Redis 8.6.2 modified=1 build, not an unmodified upstream certification.

The final eight-configuration regression passed 556/556 CTests. See the
[validation matrix](../tests/README.md#dbsize-and-stored-root-counts) for
backend/standard coverage, logs and limits.
