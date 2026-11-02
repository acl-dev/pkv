# RocksDB native storage

`HAS_ROCKSDB=ON` enables native page transactions on `rdb`. A single configured
physical RocksDB instance supports the existing List/Hash/Set/ZSet DAOs, atomic
multi-key edits, generic key operations and PKV2 page replication. The database
continues using CRC32 to choose among multiple configured path shards, preserving
the old physical placement of existing keys.

## Physical layout and compatibility

Each existing `<configured path>/rdb` directory keeps its default column family
as the logical root namespace. User keys are unchanged, including empty and
arbitrary binary keys. Other existing column families are opened and preserved.
Two private column families are added:

- `pkv.pages.v1`: bounded owned pages, plus an internal scan-schema marker.
- `pkv.scan.v1`: stable scan buckets followed by the unchanged logical key.

A page address is `BE64(owner byte length) || owner || BE64(nonzero page ID)`.
User roots are in another column family, so no prefix in the user key space is
reserved. The private pages family's empty key holds `scan-v1`; real page
addresses have at least 16 bytes and never overlap that marker. Clearing an
owner uses a range tombstone from its length-prefixed owner prefix to that
prefix's lexical successor, covering every page ID and excluding other owners.

The public structured types retain their existing bounded root/node/fragment
formats and first-access migration from JSON. No new whole-collection storage
format is introduced. Raw root replacement and deletion remove the old page
family in the same batch. Root-only TTL changes preserve pages.

On first native open, the private scan index is rebuilt from default-CF keys in
batches of up to 1024 entries. An absent marker causes an incomplete index to be
cleared and rebuilt again, without changing user roots. The ready marker is
written with WAL sync after the rebuild. Unknown marker values fail open rather
than being reinterpreted. Subsequent PKV writes maintain root/index/page changes
in the same batch. External tools modifying default-CF keys directly after this
upgrade bypass these invariants and are outside the supported writer protocol.
Older PKV binaries do not understand these extra column families/native roots;
opening this upgraded directory with an old default-only RocksDB opener is not
a supported downgrade procedure.

## Atomicity and path-shard limit

A mutation first checks every changed root and page owner. If all changes belong
to one physical RocksDB instance, one WriteBatch contains root changes, owner
range resets, touched page writes/deletes and key-index changes. A failed batch
preparation writes nothing. Logical-key locks retain the same command-level
ordering as the memory engines. Read-only source roots may span path shards
while the result is written into one physical instance.

Independent RocksDB instances have independent WALs. Mutations requiring writes
to more than one configured physical shard return `unsupported` before any
write. This includes cross-shard RENAME/SMOVE/LMOVE and multi-key updates when
both sides change. There is no distributed commit journal or recovery protocol
yet. Use one configured path for the currently complete per-instance atomic
command behavior. `rdb_native.cf` is a new single-instance example with a fresh
`./data/rdb-native` path; the existing four-shard `rdb.cf` is unchanged and no
existing shard data is silently repartitioned. This restriction is unfinished work toward the full Redis
server goal, not a claim of multi-shard transaction support.

WAL remains enabled using the existing default WriteOptions: normal writes do
not request fsync for every acknowledgement. Graceful reopen and process-crash
recovery are tested. Host/power loss durability is not established, and the
scan-schema fsync does not change normal-write durability. RocksDB I/O is still
synchronous and can block an OS worker; fiber-aware I/O offloading remains a
performance requirement for disk mode.

## SCAN

The persisted index uses 256 CRC32 buckets per physical shard. Stateless cursors
advance through `(shard, bucket)` positions. Each call copies whole buckets
through an owning RocksDB iterator; COUNT remains a hint. Keys present through
a complete iteration remain in a fixed bucket. Private page/metadata keys never
appear. Root/index changes share the WriteBatch, including raw writes and
expiry deletion. The old cursor API also now owns its iterator, distinguishes
an empty bookmark from an unstarted cursor and avoids skipping a successor when
its previous bookmark was deleted.

SCAN does not provide a global snapshot, a strict per-call work bound or proof
of race freedom. Collision-heavy buckets can return many keys. Each bucket
currently creates an iterator; reducing this overhead is possible future work.

## Build and validation

Local validation uses RocksDB 8.3.2 built as a shared library outside its source
tree, with `USE_RTTI=ON` so the PKV UBSan vptr checks can link. The dependency's
Release default disables RTTI. PKV Debug uses ASan/UBSan; the dependency itself
is a Release build. RocksDB 8.3.2 public headers require C++17, so disk validation
uses C++17 Debug/Release. Existing C++11 memory builds remain separate.

```sh
cmake -S . -B build-rocks -DHAS_ROCKSDB=ON \
  -DROCKSDB_INCLUDE_DIR=/path/to/rocksdb/include \
  -DROCKSDB_LIBRARY=/path/to/librocksdb.dylib
cmake --build build-rocks -j 4
ctest --test-dir build-rocks --output-on-failure
```

The default build remains independent of RocksDB. In a RocksDB-only feature
build, the five process-level fixtures select `rdb` and use isolated temporary
storage directories. If both oneTBB and RocksDB are enabled, those fixtures keep
the existing oneTBB selection; dedicated RocksDB storage tests still run.

`rocksdb_native_storage` covers legacy default-CF upgrade, preserved unknown
column families, binary keys/values, page/user address collisions, owner resets,
raw overwrite/deletion, interrupted index rebuild, persistent SCAN, bounded
structured values, all four collection types after reopen, exact TTL/signed zero,
same-instance RENAME and rejected cross-path writes without partial changes.
`rocksdb_crash_recovery` launches a separate writer, acknowledges complete epochs,
kills it while later multi-root/page epochs are being generated/written, then
reopens and checks both roots, all 64 page values and the key index. Six cases
pass; they establish bounded process-kill recovery evidence, not power-failure
or comprehensive storage-fault testing.

The shared server fixture now waits for a PING reply rather than just TCP
connection establishment. Replica test clients retry an asynchronously starting
RPC listener within a fixed deadline, covering disk initialization latency.

Cross-path atomic recovery, initial replica sync,
replay, disk I/O scheduling, stronger durability policy and remaining Redis
commands/services are still open.

Final validation: RocksDB C++17 Debug ASan/UBSan and Release each passed 30/30
CTests. Existing C++11/C++17 memory builds passed 28/28 each, and oneTBB variants
passed 29/29 each (174 entries across six builds). After strengthening physical
root/page-size, maximum-ID reclamation and read-only cross-shard input checks,
the two dedicated disk fixtures passed again on both disk builds. No production
code changed after the complete matrix. The original sharded `rdb.cf` was not
changed; `rdb_native.cf` adds a fresh single-path example.
