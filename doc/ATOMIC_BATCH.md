# Atomic multi-key operations

`db::transact` deduplicates byte-string keys, acquires their ACL fiber-mutex
stripes in ascending index order, reads a snapshot, and invokes one callback.
All public command reads, writes, deletions, and replication apply operations
use the same stripes. Different input key order and stripe collisions do not
change lock order. Callbacks may yield; waiting fibers release their worker
thread so the lock owner can resume.

The callback receives a non-owning `batch_view` with immutable keys and presence
flags. It can propose keep/write/erase actions, but cannot add unlocked keys.
Returning false or throwing before commit leaves all proposed values unchanged.
The view cannot be retained and callbacks/observer hooks must not recursively
access the same DB. Raw engine hooks are protected; replication uses dedicated
locked apply methods that avoid echoing writes to local observer callbacks.

A read-only transaction needs no commit capability. MGET preserves requested
order and duplicates, returns nil for missing/expired/non-string values, and
uses one expiry-time snapshot. EXISTS counts duplicate arguments separately.
These read-only operations defer physical cleanup of expired records; ordinary
single-key lazy expiration still removes them.

MSET encodes all replacements before locking. MSETNX checks every key in one
snapshot and either stages all replacements or writes none. Both clear TTL on
successful replacement. DEL preflights every selected record before deleting,
counts unique live keys, and does not partially delete valid keys when another
selected record has a decode error. Duplicate MSET keys use the last value.

## List operations

LMOVE/RPOPLPUSH select both keys under the same ordered stripe locks. Source
lookup precedes logical destination validation: a missing source returns nil
without decoding a destination payload. The page-aware DAO commits source pop
and destination push together. A drained source is deleted in that same commit.
Each surviving key keeps its own deadline; new/expired destinations start
persistent. Same-key moves retain their deadline, with no rewrite when the
selected end is unchanged or the list has one item.

LMPOP locks all declared keys, but selects the first live List in argument order,
not sorted lock order. Duplicate keys preserve selection semantics. Later
payload types are ignored after a List is selected, while storage lookup
preflight errors still abort the transaction. Results are cleared after failed
commit; no item is returned as successfully consumed. Skipped expired roots may
be deleted atomically with the selected pop.

These commands require native page transactions, currently supplied by `mdb`.
They use bounded pages rather than encoded whole-List payloads. Their PKV2
replication events apply atomically at the receiver. Snapshot/replay and native
disk support remain incomplete; see `LIST_STORAGE.md` and `PAGE_REPLICATION.md`.

## Set operations

SMOVE validates a live source and destination before checking membership, then
encodes every changed record before one commit. Same-key moves only report
membership. A missing source skips logical destination decoding. Source removal
and destination insertion are atomic when both change; the target keeps its own
TTL, and a new/expired target is persistent. A target already containing the
member stays byte-for-byte unchanged, leaving one effective source update that
can use the single-key path. Unsupported multi-change engines must reject all
other cross-key moves without consuming members.

SUNION/SINTER/SDIFF read one snapshot with every source locked. Missing sources
are empty sets, but do not suppress validation of later wrong-type/corrupt
sources. Duplicate inputs retain difference semantics (A minus A is empty).
STORE variants lock the destination too, evaluate all source snapshots before
replacing it, clear its TTL and delete it for empty results. A destination can
alias any source and need not have Set type unless it is also a source. Only
one key changes, so these STORE operations use the single-key commit primitive.
Expired source cleanup is deferred to avoid turning reads into multi-key writes.
Backend lookup preflight still covers all selected keys. These operations share
the disk/replication/observer limitations below.

SINTERCARD shares the read-only source snapshot and deferred expiry cleanup
rules. LIMIT applies after all source types are checked; no complete result Set
is constructed. SPOP uses the ordinary single-key update lock for random selection
and removal, so another SPOP/SMOVE/SREM cannot consume the same member between
selection and commit. Write/delete failures clear its sampled result. SRANDMEMBER
only reads an owning snapshot and does not rewrite bytes or TTL.

ZMPOP uses the same sorted/deduplicated multi-key locks while preserving the
request's candidate order. The first live candidate is type-checked, materialized
and popped; later values are not decoded. Backend lookup preflight still reads
all requested keys, so a backend I/O failure aborts before mutation. Skipped
expired records are logically absent with physical cleanup deferred. Exactly one
selected key is rewritten/deleted, allowing legacy single-key commit primitives.
Failure clears both selected key and popped values; live TTLs are retained.
This gives command-level synchronization with existing mutators and replication
apply under the shared lock discipline, not a new durable/replicated commit
protocol. Concurrent tests cover 640 unique members across overlapping key lists.

Sorted Set union/intersection/difference and their STORE variants use the same
source/destination snapshot. Set inputs are valid with implicit score 1. Repeated
keys share one owning view while each argument keeps its weight. Every source is
validated before the option callback runs under locks; that callback must not
perform I/O or recurse into the DB. STORE proposes only one destination write or
delete, overwrites its type and clears TTL. Read variants retain source bytes;
expired physical cleanup is deferred. ZINTERCARD probes owning views without a
result map and stops at LIMIT after complete source validation. Member visitors
also let SINTERCARD avoid copying the candidate Set. Backend lookup/encode/commit
failures preserve the destination and clear output/count; custom visitor/lookup
errors are treated as invalid stored data. These paths retain the engine,
replication and observer limits described below.

RENAME/RENAMENX propose a destination write plus source erase in one transaction.
They transfer exact source bytes/expiry after payload validation; no encoding is
needed. Native mdb commits both changes or neither; engines without dbapply reject
the move, while same-key/NX-blocked no-ops remain valid. Shared locks make MGET
observe the value at exactly one name during a rename. Nonmoving/error paths
defer physical expiry cleanup. See [key transfer rules](KEY_TRANSFERS.md) for
corruption handling and unfinished logical DB/COPY requirements.

COPY reuses the transaction for same-DB operations. Across DBs, `copy_to` locks
DB identities in total pointer order and both key stripes, and permits only a
destination mutation. The source is never deleted; a single backend write commits
the copy. Different codecs convert through a logical value before commit, while
identical codecs retain exact bytes. This avoids opposite-direction COPY
deadlocks in tested memory execution and leaves sources unchanged on failure.
Legacy peers still receive DB 0 only; version 1 peers receive DB-scoped live
updates, without a replicated transaction envelope or initial/reconnect replay. See [logical database limits](LOGICAL_DATABASES.md).

## Engine commit contract

A single effective change can use the engine's existing single-key primitive.
Multiple changes require `dbapply` to commit every action or leave every value
unchanged on failure. Its default implementation returns unsupported; there is
no sequential-write fallback. The command returns an explicit unsupported-backend
error rather than pretending to provide atomic multi-key writes.

The built-in `mdb` backend implements the contract across its memory shards:

1. Acquire affected logical shard locks in sorted order.
2. Allocate replacement strings, any new sub-bucket maps, and prepare insertions.
3. Insert new nodes, rolling back every new node if any allocation fails.
4. Once no allocations remain, swap existing values and erase deleted keys.

Each logical shard owns 256 stable, lazily allocated unordered_map buckets.
Bucket creation occurs before visible key insertion; allocation failures may
leave empty bucket objects but cannot expose new keys. Bucket objects remain
stable for replacement/rollback pointers while shard locks are held. Native
SCAN releases those locks before DAO key lookups, preserving lock ordering.
The memory maps use owning standard strings and RAII lock/resource ownership
so allocation exceptions can be handled. These locks protect memory operations
only; no coroutine yield or I/O occurs while a native shard spinlock is held.
Observers are notified only after successful batch commit. Notification streams
are not themselves transactional and observer reentrancy remains restricted.

`mdb` and oneTBB implement atomic in-memory multi-change/page commits. RocksDB
now implements root/page/index WriteBatch commits within one physical instance;
mutations spanning independent configured instances are rejected before writes.
WiredTiger supplies snapshot root/page/index commits within one connection,
also rejecting cross-path mutations before writes. Older ACL engines still need
native multi-change/page support. See [RocksDB](ROCKSDB_STORAGE.md) and
[WiredTiger](WIREDTIGER_STORAGE.md) for disk layout and recovery scope. Common key locks alone
do not provide cross-database disk atomicity or power-loss durability. PKV2
preserves native page-transaction boundaries; legacy whole-root observer events
can still be delivered individually rather than as one command-level batch.

## Validation

`batch_allocation_failure` injects allocation failures only during native memory
commit. It checks every failure point until success, with existing values, new
keys, and deletions in one batch. The current local run covers 51 batch failure points,
including lazy bucket preparation. It also injects each of 15 allocation failures
in a native SCAN page, requiring cleared partial output/cursor and unchanged
stored keys. Counts depend on the allocator/library and are not API guarantees.

Storage tests check callback aborts/exceptions, unsupported backends, failed
lookups/commits, corruption preflight, custom codecs, deferred expiry cleanup,
observer behavior, and replication apply waiting behind a yielded batch owner.
Four threads with 32 fibers perform 640 two-key transfers, alternating key order
and yielding while locked. Socket tests check coherent MSET/MGET snapshots,
MSET/DEL snapshots, and overlapping MSETNX races in addition to ordinary replies.
These validate the tested memory backend; they do not establish disk durability,
full replication consistency, or a whole-server performance advantage.

## Cluster slot correction

Routing now uses Redis CRC16/XMODEM modulo 16384 with full byte lengths. It uses
the substring between the first `{` and its following `}` only if nonempty; an
absent, empty, or incomplete pair means hashing the entire key. Multi-key commands validate every key and
return CROSSSLOT before executing across different slots. CLUSTER KEYSLOT is
implemented, and CLUSTER commands report disabled support when cluster mode is
off. Tests compare fixed and randomized binary keys with Python's CRC16 and a
local Redis build, plus same-slot and cross-slot requests on unassigned nodes.

Earlier pkv routing used CRC32 and C-string key lengths. Existing clusters using
that placement cannot safely perform a rolling upgrade just by replacing the
binary: keys must be redistributed offline according to the corrected slots,
with a tested backup and migration plan. This change does not migrate existing
cluster data or topology files. Concurrent topology updates, slot migration,
MOVED/ASK flows, and failover still need full cluster integration tests.
