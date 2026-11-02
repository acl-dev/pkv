# HGETDEL: atomic Hash field retrieval and deletion

PKV supports `HGETDEL key FIELDS numfields field [field ...]`, checked against
local modified Redis 8.6.2 source and an isolated instance of that binary.
The reply is an array in request order: a bulk value for a live field and null
for a missing field. Duplicate requests return the value once and null for
later occurrences. Empty names/values, embedded NULs and non-UTF8 bytes retain
their lengths. Removing the last field removes the key and its page family;
otherwise the existing absolute key deadline is retained.

## Native storage and atomicity

The server uses `paged_hash` and the existing PMR root/AVL-node/blob format.
One key lock covers type validation, argument parsing, field retrieval,
deletions and one native page commit. Reads and mutations cannot be split by a
concurrent writer. All result values remain private until commit succeeds;
read, allocation, encoding or commit failure discards staged changes and clears
the DAO output. WATCH sees actual removals and drained roots. A missing-field
no-op does not dirty WATCH.

`map_store::take` reads a matched node's value during the AVL deletion traversal.
It does not perform a separate value lookup and deletion search. Rebalancing,
successor movement, root element counts and page-family cleanup use the existing
transaction staging. Existing erase callers pass no value destination and keep
their prior behavior. The operation reads requested values for the reply, not
the entire Hash or unrelated large value blobs. Roots and stored pages retain
the existing 16-KiB bound. A large requested field can still require a large
response allocation; there is no claim of constant response memory.

Legacy JSON Hashes are imported into native pages inside the same transaction.
A rejected parser/type/storage operation cannot partially publish migration.
The explicit `json_hash` fallback also implements atomic retrieval/removal for
old JSON records. That fallback materializes its old representation and retains
its existing format limitations; the public server path and newly written Hash
records use bounded native pages. No new stored-format or replication version,
external dependency, or durability policy is introduced.

## Parsing, routing and transactions

Root arity is checked before MULTI queuing. For a sufficiently long request,
Redis checks the existing key type before FIELDS/count syntax. The parser runs
under the same key lock after type validation to preserve that precedence
without a racy probe. FIELDS is matched case insensitively by complete length.
Counts must be canonical positive integers in the platform's signed long range
and exactly match the remaining arguments. Reflected field bytes do not appear
in these diagnostics. PKV rejects NUL-suffixed FIELDS tokens; the local reference
can accept a C-string prefix, so strictness cases are independent assertions.

Only the Hash key participates in cluster routing and COMMAND GETKEYS metadata.
Hash tags in fields do not contribute slots. SELECT and source EXEC use the
existing logical-database/workspace selection. HGETDEL queues like other Hash
mutations; body errors remain individual EXEC replies, while bad root arity
aborts the transaction. Successful source page batches use existing replication
boundaries, including native source EXEC batches. This does not add initial sync,
durable replay, receiver promotion or a durable outbox.

## Verification scope

The C++ fixture checks missing keys, request-order duplicates, empty/binary
fields and values, partial/draining deletes, deadlines, one-commit behavior,
failed reads/commits, corrupt/future records, legacy migration and parser
failure preserving original legacy bytes. Eight concurrent callers must return
one coherent two-field winner. Native and explicit JSON allocation-failure
sweeps require unchanged fields and deadline on every failure and correct
remaining fields/deadline on success.

The bounded-work fixture creates 64 unrelated 50-KiB fields and one small target.
Deleting the target and requesting missing/duplicate fields runs with a 32-KiB
per-allocation ceiling, reads no unrelated value blobs and commits once. This
is a structural/scaling check, not a whole-server throughput measurement.
Native oneTBB/RocksDB/WiredTiger fixtures check stored content and TTL; disk
backends reopen in a separate process.

TCP tests compare 412 ordinary replies and later state with local Redis,
including randomized mutations, binary values, expiry, type/syntax precedence,
count/arity errors, logical DBs, source EXEC and COMMAND discovery. HGETALL is
compared as an unordered field/value map; HGETDEL reply order is compared
exactly. Separate assertions cover strict NUL options, framing, WATCH no-ops /
removals / draining, an eight-client race and cluster routing. Live source and
receiver checks require a large-field source EXEC update, retained TTL and
physical root removal after draining, observed through DBSIZE. Receiver
readiness is established before mutation; these are not initial-sync tests.

Hash field expiration and HGETEX/HSETEX remain unfinished. HGETDEL applies the
current key-level expiration model. The tests do not establish complete Redis
compatibility, disk crash safety, whole-server race freedom or faster throughput
than stock Redis.


The final implementation passed 670/670 full-matrix CTests in all eight cached
memory, oneTBB, RocksDB and WiredTiger configurations. Each passed 412 common
TCP comparisons plus separate strictness, WATCH, concurrency, cluster and
replication assertions. Both allocation sweeps reached success after 77 native
and 12 JSON failure points. Debug builds use ASan/UBSan. See retained
[counts, durations, hashes and limitations](
benchmarks/hgetdel-validation-2026-10-31.json).
