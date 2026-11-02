# Redis compatibility work

Current `mdb`/`mdb_tbb` and single-instance `rdb`/`wdb` use bounded pages for
String, List, Hash, Set and ZSet, including ZSet member/score indexes. Legacy
JSON sections below
describe compatibility formats/fixtures. Structured commands migrate old values
atomically; TYPE/TTL/COPY/RENAME/SCAN and PKV2 understand page ownership. Other
engines explicitly reject these commands until native page transactions exist.
See [ZSet integration](ZSET_STORAGE.md) and
[RocksDB atomicity/recovery limits](ROCKSDB_STORAGE.md) and
[WiredTiger storage/recovery limits](WIREDTIGER_STORAGE.md).


This is an implementation roadmap, not a claim that pkv is already a complete
Redis server. Implemented core replies now support RESP2 and negotiated RESP3; see
[protocol scope](RESP3.md). Persistence interoperability, replication, cluster
behavior and unsupported command families still need separate validation. Performance claims require reproducible measurements with
matching command semantics, durability settings, payloads, and concurrency.

## Current cross-DB status

[Public MOVE](MOVE.md) now connects native memory/disk coordination, paired
local effects and PKV4 publication. Native storage defaults to an automatically
bound decision journal. Disk mixed-group replay, initial synchronization and
PSYNC remain unfinished. The [current live name inventory](
benchmarks/public-move-command-inventory-2026-11-02.json) records 231 PKV names
against 412 local-reference names: 182 remain absent and cluster|addslave is
an extra alias. Name coverage alone is not semantic compatibility.

The implementation checkpoints below preserve the scope and counts of their
linked historical reports. Statements about missing MOVE in those checkpoints
refer to those earlier source snapshots, not the current public command.
Current MOVE validation is documented separately in [MOVE](MOVE.md).

## Command discovery

COMMAND now exposes the registered commands, ten-field INFO metadata, filtered
LIST and argument-only GETKEYS/GETKEYSANDFLAGS. COUNT, HELP and basic DOCS are
available. DOCS currently includes only summary/group; ACL category metadata
does not imply ACL authorization. See [scope and validation](COMMAND_METADATA.md).

## Verified foundation

The independent `tests/` suite checks RESP2 framing, binary payload transport,
empty versus null values, incremental parsing, pipelining, integer and length
validation, parser resource limits, and object ownership during early teardown.
The full build now links a reusable `pkv_core` and enables storage fixture tests
and real loopback socket tests. See `tests/README.md` for commands and limitations.

Verified command paths on the two-thread `mdb` server: SET/GET with NX/XX, GET,
KEEPTTL and EX/PX/EXAT/PXAT, PING, ECHO, wrong-type GET, arity errors, continued
processing after
command errors, ordered pipelines, fragmented requests, and QUIT. Tests cover
empty keys/values, all byte values including non-UTF-8 data, and eight concurrent
clients on separate keys. The ordinary socket suite now contains 32,155 assertions/scenarios per server across
string/hash/key, List, Set and Sorted Set groups and has also passed against a local Redis 8.6.2
build.
This is a bounded compatibility check, not full Redis parity or a benchmark.

The suite now also covers HSET/HGET/HDEL/HMSET/HMGET/HGETALL with binary fields,
new-field counts, duplicate fields, request ordering, nil versus empty values,
wrong-type protection, TTL retention, and deletion of the last field. HGET uses
length-aware lookup without allocating/sorting an entire field map. TYPE returns
Redis names or `none`; DEL counts existing supplied keys and ignores duplicates
and expired keys; basic EXPIRE validates 64-bit integers and deletes immediately
for non-positive durations. PEXPIRE/PEXPIREAT, PTTL, EXPIRETIME/PEXPIRETIME,
PERSIST, and EXPIREAT
are also implemented, including NX/XX/GT/LT expiry conditions and exact
millisecond deadline preservation. Twenty concurrent NX races each have exactly
one winner; 200 concurrent SET GET exchanges form a complete chain without
repeated or lost old values. SET comparison options IFEQ/IFNE/IFDEQ/IFDNE,
DIGEST and DELEX are now implemented; see [comparison options](SET_COMPARISON.md)
and [streaming string digests](STRING_DIGEST.md). Further string coverage remains
part of the compatibility goal.

String coverage also includes INCR/DECR/INCRBY/DECRBY, APPEND, STRLEN,
GETRANGE/SUBSTR, SETRANGE, GETDEL, GETEX, GETSET, SETNX, and SETEX/PSETEX.
Mutations share the per-key lock, preserve TTL where Redis does, reject signed
integer overflow, and check the 512-MiB growth limit before allocating. Tests
exercise binary ranges, negative indexes, empty values, wrong-type/error
precedence, failed storage writes, and unchanged values/deadlines after errors.
Eight concurrent clients verify all 800 INCR results, complete APPEND blocks,
non-overlapping SETRANGE writes, and exactly one GETDEL consumer. Command handlers
and arity rules share one registry. Native STRLEN reads bounded metadata, while
GETRANGE touches the intersecting chunks. Legacy JSON values migrate under the
key lock before single-key operations; that first access retains JSON parsing
cost. MGET leaves legacy records unchanged. See `STRING_PAGES.md`.

Eight additional pkv-only assertions check relative expiry overflow and retention
of the old value/TTL after rejection. The local Redis 8.6.2 reference build
(reports modified=1) accepts PSETEX with a duration of 9223372036854775807 ms;
its source adds the current time to a signed integer without a pre-addition
overflow guard. pkv deliberately rejects that duration, including equivalent SET
and GETEX options, instead of reproducing signed overflow. This boundary is kept
separate from the 6,525 common assertions; it is a documented compatibility
difference, not a passing comparison against Redis.

Hash coverage now includes HSETNX, HINCRBY, HLEN, HSTRLEN, HEXISTS, HKEYS,
and HVALS. Conditional insertion and integer increments run under the same key
lock as HSET/HDEL and preserve the key deadline. Rejected non-integer fields,
signed overflow, and failed storage writes leave the encoded record unchanged.
Eight clients verify 800 HINCRBY results and 20 HSETNX races with one winner each.
HMGET reads requested fields from one owning view and preserves argument order,
duplicates, and nil placeholders without constructing a complete sorted map.
HLEN and HSTRLEN/HEXISTS use codec size/field-length accessors; the JSON codec
avoids additional field-value copies, while decoding still parses the full JSON
record. Default accessors keep existing codec implementations source-compatible.
A read-only test codec rejects full materialization to verify selective HMGET and
field-length fallback behavior. HRANDFIELD now samples native field ranks,
optionally reading the matching value in the same traversal, without scanning
all fields for a small sample. See [Hash sampling](HASH_SAMPLING.md).
HINCRBYFLOAT and INCRBYFLOAT now use long-double calculation and fixed
fractional formatting, with native point-field Hash updates and final-value
replication; see [floating increments](FLOAT_INCREMENTS.md).
HGETDEL is now implemented; see [atomic field retrieval](HGETDEL.md). Hash
field expiration and HGETEX/HSETEX remain unimplemented; key TTL
coverage does not establish field TTL compatibility.

List support now includes LPUSH/RPUSH, LPUSHX/RPUSHX, LPOP/RPOP with optional
count, LLEN, LRANGE, LINDEX, LSET, and LTRIM. These use per-key atomic updates,
preserve existing key deadlines, keep binary elements and duplicate ordering,
and delete empty keys. TYPE, EXISTS, DEL, expiry/PERSIST, MGET wrong-type nils,
and other string/hash wrong-type checks recognize the new type. The socket suite
adds 2,002 List assertions per server: signed range/index boundaries, lookup and
argument-error precedence, RESP2 nil arrays versus nil bulk strings, exact TTL
retention, a 300-step state model, 320 atomic two-element pushes and 640 items
consumed exactly once by eight clients. Codec/storage fixtures also check
corruption retention, unsupported custom codecs, failed I/O and failed deletion.

Fourteen pkv-only List safety assertions validate full-width LSET index rejection
and unchanged values/TTLs. The local Redis reference narrows the parsed `long`
index to the `int` parameter of `listTypeReplaceAtIndex`; extreme indexes can wrap
and modify an unrelated element. pkv intentionally retains 64-bit range checks
rather than reproducing that behavior; these checks are separate from the common
List comparison. The reference binary reports modified=1, so this observation is
specific to that local build/source rather than a claim about every Redis build.

List persistence uses the versioned codec's new `L` type; previous builds cannot
read it and legacy export is rejected. See [storage format](STORAGE_FORMAT.md).
The JSON/vector implementation still parses and rewrites whole records, so the
commands do not yet meet Redis's list performance characteristics. Blocking
commands and waiter wakeup/cancellation remain unfinished.

LINSERT, LREM and LPOS are now implemented. LINSERT uses the first binary-equal
pivot; LREM removes bounded matches from either end (or all matches for count 0)
with stable linear compaction, deleting the key if drained. Both retain key TTL
and avoid writes for missing pivots or unmatched removals. LPOS supports repeated
RANK/COUNT/MAXLEN options, reverse searches, unlimited count 0, bounded scans,
and integer versus nil versus integer-array replies. Signed-minimum ranks/counts
are rejected before negation, matching the reference's explicit range checks.
A further 2,710 assertions per server cover duplicate/binary elements, full option
matrices, syntax/error order, a 280-step model, 240 concurrent insertions and 128
matching elements removed exactly once by eight clients. Storage fixtures cover
failed writes/deletion, lookup errors and corruption retention.

LPOS uses a codec accessor with a fallback for existing custom List views. The
JSON implementation compares decoded strings directly without copying the entire
list, and range/index reads use direct flat-array indexing. List decode validates
that every array element is a string before exposing this view. These changes
reduce extra traversal/copying after parsing; the complete JSON record is still
parsed for every read. Codec tests include a selective view that rejects full
List materialization and verify LPOS still works.

LMOVE, RPOPLPUSH and LMPOP now use the common multi-key transaction API.
LMOVE validates both lists and encodes all changes before one commit; a rejected
destination, codec failure or failed native batch leaves both keys intact.
Same-key moves rotate without deleting/recreating the key. Existing source and
destination deadlines stay with their keys; a new or expired destination starts
without expiry. LMPOP selects the first live list in request order (including
duplicate keys), returns a nested key/items array, and ignores later wrong-type
payloads after finding a match. Its read preflight still covers all selected
storage keys, so a backend I/O failure anywhere aborts the snapshot. Expired
unselected keys are not physically deleted by this operation.

A separate 2,963-assertion List transaction suite passes on each server. It covers
four move directions, same-key rotation, exact deadlines, binary values, argument
and wrong-type errors, a 240-step model, 480 values moved exactly once, 480 further
opposing moves without loss, and 480 values consumed exactly once by concurrent
LMPOP. Storage fixtures cover second-record encode failure, failed commit/read,
unsupported multi-change backends and failed LMPOP writes/deletion. Cluster
routing checks both move keys and exactly the declared LMPOP keys. Cross-key
moves remain unsupported on engines without native multi-change commits; LMPOP
changes one selected key and can use the single-key commit path. Whole-record
JSON costs, disk crash atomicity and replicated batch boundaries remain open.

Set support includes SADD, SREM, SCARD, SISMEMBER, SMISMEMBER, SMEMBERS,
SMOVE, SUNION/SINTER/SDIFF and their STORE variants. Members are binary strings;
empty/numeric-looking values remain distinct, duplicates are counted once, and
removing the final member deletes the key. SADD/SREM/SMOVE preserve surviving
key deadlines. STORE commands replace any destination type, clear its TTL and
delete it for empty results, including when the destination aliases a source.
All algebra sources are validated before any write, even when an early missing
source makes the intersection empty. Read-only algebra uses one locked snapshot
and defers physical expiry cleanup; backend lookup errors anywhere abort it.

SMOVE uses the common native batch contract for two changed keys. Missing sources
return 0 before logical destination decoding; a live source checks the destination
type even when the requested member is absent. Same-key moves are no-ops. If the
target already contains the member, only the source needs a commit; otherwise
engines without native multi-change support reject the operation without consuming
it. Algebra STORE changes one destination, so it can use the single-key commit
path under locks on every source and destination.

A 3,257-assertion Set suite passes on each server. It covers command errors,
request ordering, duplicates, binary members, exact deadlines, source/destination
aliasing, a 280-step model, 320 independent additions plus 16 shared members,
competing removals and 640 two-way SMOVE operations. Concurrent SUNION/SINTER
readers see all 320 members exactly once across the two sets and an empty
intersection. Codec/storage fixtures cover invalid/duplicate/future payloads,
unsupported codecs/backends, failed reads/writes/deletion, second-record encoding
failure and failed batch commits, including preserved values/TTLs and cleared
failure results. Cluster checks include all Set commands and cross-slot rejection.

Sets now use bounded native Map membership pages on `mdb` and `mdb_tbb`.
Legacy v1 `T: "U"` records migrate atomically on first Set DAO access. SCARD
reads native root metadata; membership probes edit/read individual AVL nodes.
Migration retains the existing validated JSON membership view until commit,
while ordinary native commands do not parse or rewrite whole Set records.
See [native Set layout and validation](SET_STORAGE.md). Other engines reject
Set operations until they implement page transactions.

SPOP, SRANDMEMBER and SINTERCARD add 728 common assertions per server. Positive
sampling counts return up to the requested number of different members; negative
SRANDMEMBER counts return exactly their magnitude with replacement. Missing keys,
zero counts, singleton empty strings, syntax/type precedence and signed extremes
have reference coverage. SPOP selects and removes under one key lock, retains
TTL when members remain, and deletes drained keys. Failed storage writes/deletion
clear the result and leave encoded members/deadlines intact. Eight clients pop
480 unique members exactly once. Random outputs are checked for membership,
length, uniqueness rules and remaining contents, not equality with Redis draws.

The non-cryptographic generator is independent per worker thread. Sampling uses
uniform integer distributions and partial Fisher-Yates without replacement, not
modulo reduction. Fixed-seed helper tests check all six two-member subsets of a
four-member population, replacement frequencies and representational bounds;
these are regression checks, not a security or statistical certification.
Native SINTERCARD validates all source types before counting, uses the smallest
candidate and stops at LIMIT without constructing an intersection result.
Native sampling uses subtree ranks and sparse numeric Fisher-Yates state rather
than indexing the complete population for small samples. Legacy migration still
processes the whole old record. Complete-command constant-time performance is
not established.

Six pkv-only assertions check rejection of a negative SRANDMEMBER count whose
magnitude exceeds the response vector's representable size. It returns
`ERR requested sample is too large`, preserves members/TTL and keeps the connection
usable. That enormous allocation request is not sent to the reference Redis
process and is not counted as a successful comparison. Large representable
replies still depend on available memory; a configurable whole-server reply
budget and broader out-of-memory recovery remain unfinished.

Stateless SCAN, HSCAN and SSCAN add 141 socket scenarios per server; ZSCAN adds
70 more, with flat member/score pairs and MATCH/COUNT options.
SCAN supports MATCH/COUNT/TYPE on the native `mdb` backend. HSCAN adds NOVALUES;
SSCAN supports MATCH/COUNT. Cursors are portable across connections, pagination
continues through empty filtered pages, and scans cover stable keys/members while
transient data changes. Binary wildcard, bracket/range/inversion/escape behavior
has reference coverage. Errors return replies without closing the connection.
Eight pkv-only checks reject NUL-suffixed cursor strings instead of reproducing the
local reference's strlen truncation. Backend scan failures and corrupt records
also have direct tests. The older per-connection scan fallback has been removed.

Native memory shards now contain 256 stable, lazily allocated scan buckets;
small pages stop at bucket boundaries rather than copying an entire shard.
Collection pages also use stable hash buckets and owning codec accessors. These
preserve continuation under rehashing or surrounding-member mutation, but strict
work bounds are not established: collision-heavy native buckets can still hold
native locks. Native Hash/Set scan reads bounded member pages; legacy views
still decode whole JSON records; native Hash/Set/ZSet scan reads pages.
COUNT remains a hint. Other engines explicitly reject the new stateless scan
API until implemented. Global snapshot consistency, bounded work and production
scan performance are not established. See [scan semantics and limits](SCAN.md).

Sorted Set foundation now adds ZADD, ZINCRBY, ZREM, ZCARD, ZSCORE/ZMSCORE,
ZRANK/ZREVRANK (WITHSCORE), rank-based ZRANGE/ZREVRANGE (WITHSCORES and ZRANGE
REV), and ZPOPMIN/MAX. ZADD supports NX/XX/GT/LT/CH/INCR. A 2,259-assertion
socket group per server covers conditional updates, duplicate arguments, input
preflight, score ties and binary ordering, signed range boundaries, exact TTL
retention, a 200-step model, 800 atomic increments, 240 concurrent additions and
320 unique pops. Codec/storage tests check v1 `Z` records, infinities/subnormals,
NaN rejection, unchanged data on failure, custom fallbacks and 10,000 bit-exact
double text round trips. Parsing uses a C numeric locale; fpconv formatting is
vendored with its Boost license. Negative-zero text is encoding-dependent in the
reference, so the compact-zero scenario compares numeric zero explicitly, not
universal reply-byte equality. pkv preserves signed zero.

The owning Sorted Set view builds score order lazily and safely for concurrent
readers. Generic expiry/TYPE/SCAN TYPE recognize the new kind. JSON still parses
whole records and mutations rewrite them; this does not establish Redis's ordered
storage time complexity. ZRANGE/ZRANGESTORE now support BYSCORE/BYLEX,
REV and LIMIT; legacy score/lex range aliases, ZCOUNT/ZLEXCOUNT and all three
ZREMRANGEBY modes are covered by 2,458 further assertions per server. Range
stores lock source and destination together, replace the destination with no TTL,
and delete it for an empty result. Lex compatibility uses equal-score sets, as
Redis only defines lex results under that condition. ZSCAN is now supported via an optional selective codec accessor,
with a materializing fallback. Scans retain live record bytes/TTLs and do not
build score order. ZUNION/ZINTER/ZDIFF, their STORE variants and ZINTERCARD now
accept mixed Set/Sorted Set sources, with repeated weighted contributions,
SUM/MIN/MAX, WITHSCORES and LIMIT as applicable. Source validation precedes option
parsing. STORE supports source aliases, clears the destination TTL and deletes
empty results. A 1,821-assertion algebra group and 600 additional live paired
floating comparisons cover those paths; visitor/lookup codec access avoids full
source-map copies, and ZINTERCARD/SINTERCARD stop membership traversal at LIMIT.
Full JSON decode and result-map work remain, and floating ordering on different
C libraries/platforms is unverified. Blocking pops remain unfinished.
ZMPOP now supports MIN/MAX and optional COUNT over request-ordered keys, with a
nested member/score array reply and nil array when no candidate exists. Shared
multi-key locks make selection/removal atomic; only the first live candidate is
mutated, TTL is retained, and draining deletes the key. A 2,268-assertion group
covers priorities, duplicates, wrong types, signed parser bounds, a 260-step
model and eight clients consuming 640 members exactly once. Storage tests cover
legacy one-write backends and cleared output on read/write/delete/encode failure. ZRANDMEMBER adds optional count and
WITHSCORES, with unique positive-count samples and replacement negative-count
samples. Its owning-view accessor avoids materializing another member map and
shares the existing Set sampling algorithm. The suite adds 908 common assertions
and 10 independent pkv-only oversized-output checks. Live bytes/TTLs are preserved,
and concurrent sampling observes one source population. Previous builds cannot read this new v1 type, and legacy export is
rejected. See [Sorted Set semantics, format and remaining work](SORTED_SETS.md).

The logical record and owning decoded-view interfaces no longer expose yyjson
types in DAOs. The DB selects a shared immutable codec at open time, and tests
run the same DAOs against an independent non-JSON string codec. Hash single-field
reads retain lazy lookup rather than constructing all fields.

The legacy JSON writer emits explicitly versioned v1 records with exact
millisecond expiry. Legacy unversioned records remain readable; mutated legacy
records are rewritten individually with their existing logical deadline. Unknown
future versions are preserved and reported as unsupported. There is no bulk
migration. See [storage format, upgrade and rollback policy](STORAGE_FORMAT.md):
older readers only see a rounded-up compatibility timestamp, so production
rollback requires a tested backup or explicit offline export. The legacy export
encoder rejects sub-second precision loss. No binary replacement format has
been selected. A new DB lookup status distinguishes found/missing/error, and
atomic update
callbacks propagate failed lookups rather than overwriting unreadable data.
RocksDB and WiredTiger supply that distinction; legacy engines using the
default `dblookup` adapter still map a failed boolean read to missing and need
engine-specific status propagation. Both optional disk implementations have
local build, native storage and real-server runtime coverage; see the validation
section and backend-specific recovery limits.

RENAME/RENAMENX atomically transfer all supported types, preserve logical
payload/expiry and overwrite the destination as applicable. Enumerable legacy
values now import into bounded native pages; opaque extensions preserve raw
encoding. A 2,696-check
socket group covers type/TTL/alias/overwrite behavior, a 200-step model, twelve
NX races and coherent MGET snapshots during renames. Payload validation rejects
corrupt/future sources; no codec encode is required. Native mdb commits the
write/delete together; unsupported engines fail without partial moves. SELECT/COPY now use connection-local
selection and separate lazy DB instances; databases defaults to 16, distinct from
db_count physical shards. The suite adds 1,341 ordinary assertions and nine custom
configuration checks per endpoint. COPY supports DB/REPLACE, both codec-preserving
and transcoding paths, and fixed-order cross-DB locks. Nonzero DB live updates now use negotiated version 1 database identity. MOVE,
initial/reconnect synchronization and broader database lifecycle commands remain
unfinished; legacy subscribers still receive DB 0 only. See [logical DB limits](LOGICAL_DATABASES.md).
See [key-transfer details and DB requirements](KEY_TRANSFERS.md).

The legacy DB 0 update stream now handles empty/binary keys in single and batched
frames, rejects invalid/truncated headers, and clears failed outputs. Failed
flushes release their batch once and drain queued references. Subscriber lists
are protected and idle subscribers use a fiber-aware queue. One publication FIFO
preserves same-key commit order across workers, and reads no longer produce
stream records. New transport fixtures plus 420 live-stream assertions cover
those paths, including 400 ordered increments during connection resets. This is
custom pkv RPC validation, not Redis replication parity. Version 1 negotiation now carries DB identity, with pool-scoped apply and legacy
DB 0 fallback. The suite adds 18 upgrade, 12 two-process apply and eight reconnect
checks. Initial sync, replay and recovery remain unfinished; see
[replication scope](REPLICATION.md).

## Work remaining, in priority order

1. Broaden the socket suite into command-by-command Redis differential tests,
   including further data types, expiry, and negative cases. MGET/MSET/MSETNX,
   EXISTS, atomic multi-key DEL, coherent snapshot reads, and overlapping MSETNX
   races now have memory-server and local Redis coverage. A separate cluster
   routing suite adds 338 slot/error assertions per server.
2. Finish byte-length propagation beyond the verified string/map path and
   validate each backend. Reconcile documented strict SET option differences,
   unsupported engine batches, stateless SCAN coverage and bounded scan work.
   Replace generic fallback errors with precise
   command-specific Redis errors.
3. Complete engine-specific error propagation and batch commit support. The
   common transaction API locks all selected key stripes in sorted order. The
   built-in `mdb` engine commits multiple changes with allocation-failure rollback;
   other engines explicitly decline unsupported multi-change commits. Single-key
   commands and replication apply use the same locks. Tests include yielded
   single-key updates and 640 two-key transfers from 4 threads / 32 fibers,
   callback errors, failure preflight, and 51 native batch and 15 native scan allocation-failure points.
   Read-only MGET/EXISTS snapshots defer physical expiry cleanup. These are
   in-process memory guarantees; disk crash atomicity, replicated batch
   boundaries, observer reentrancy/order, and topology races remain unfinished.
   See [atomic batch and cluster migration details](ATOMIC_BATCH.md).
4. Extend the codec/value model to further Redis data types. The codec boundary,
   version dispatch, legacy reads, millisecond deadlines, and injectable codec
   selection are implemented. Add alternative encodings only with realistic
   measurements, bounded decoding, corruption/fuzz tests, and an explicit
   migration/rollback policy. Measure full server and backend costs separately
   from the optional codec microbenchmark.
5. Extend strings and key commands, then hashes, lists, sets, sorted sets,
   transactions, scripting, Pub/Sub, streams, and client/server administration.
   Track every command against an explicit Redis version and behavior tests.
6. Validate durability, replication, cluster routing/failover, authentication,
   ACLs, resource limits, observability, and recovery under faults.
7. Run differential, randomized, concurrency, long-running, and performance
   suites before calling a compatibility milestone complete.

## Local validation (2026-10-30)

Full memory-server builds now need only ACL and yyjson. CMake accepts a local
yyjson checkout through `PKV_YYJSON_SOURCE_DIR`; it builds that source into the
selected build directory. RocksDB, WiredTiger, oneTBB, and jemalloc are opt-in
and fail clearly during configuration if explicitly enabled but unavailable.
No external source checkout or installation was modified for this validation.

C++17 Debug with ASan/UBSan (UBSan configured to halt on error) and C++11 Release
both passed all nine CTest entries (four RESP variants, native allocation-failure
injection, record-codec tests, storage, socket compatibility, and cluster routing). Startup now uses C++11 function-local
singleton instances after
the strict sanitizer run exposed null-reference binding in the prior ACL singleton
template. The tested built-in memory backend now has single- and multi-key
concurrent read-modify-write coverage. Subsequent C++11/C++17 matrices also
exercise oneTBB native batches, Hash/List/Set/ZSet page DAOs and actual replication.
Before HRANDFIELD, the full suites had 28 tests without oneTBB and 29 with it. Cross-path disk
atomicity, general race freedom, topology/failover, durability and full
server performance remain unproven. Existing CRC32-based pkv clusters require offline data redistribution
before switching to the corrected Redis slot algorithm; no migration was run.
Replacing C++ variable-length stack arrays with owned heap buffers also permits
strict Clang builds and avoids payload-sized RPC stacks.

RocksDB-only C++17 Debug ASan/UBSan and Release builds now each pass 30 CTests,
including real disk-backed command/cluster/replication fixtures and killed-writer
recovery. Existing memory C++11/C++17 suites retain their 28/29-test counts.
WiredTiger-only C++17 Debug ASan/UBSan and C++11 Release builds add the same
30-test scope with native snapshot transactions and binary-key persistence.
Cross-path disk atomicity, power-loss guarantees and initial replica
synchronization remain unfinished.

The final eight-build matrix passed 234 CTest entries: memory 28 twice, oneTBB
29 twice, RocksDB 30 twice and WiredTiger 30 twice. WiredTiger sync-heavy socket
coverage uses a 180-second timeout and took about 161/163 seconds with the two
matrices running concurrently. No server performance superiority follows from
these correctness/recovery checks. See `WIREDTIGER_STORAGE.md` for details.

HRANDFIELD extends the matrix to 242 passing entries across the same eight
builds (memory 29 twice, oneTBB 30 twice, disk 31 four times). The final common
Hash routing adjustment was followed by rebuilding all eight and rerunning
their sampling/routing tests, adding 16 passing entries. See
[Hash sampling validation](HASH_SAMPLING.md) for coverage and limits.

Floating increments add two fixtures per build, bringing the current matrix to
258 passing entries: memory 31 twice, oneTBB 32 twice, disk 33 four times. One
RocksDB Debug socket timeout occurred under concurrent disk workloads; an
isolated full rerun passed under the original timeout. See
[floating-increment validation](FLOAT_INCREMENTS.md) for exact scope and limits.

A native String component now provides PSR inline roots and 16-KiB chunks,
localized ranges/edits, zero-filled growth, TTL and independent page clones.
The default String DAO now uses PSR roots/pages, with generic metadata, key
operations, atomic legacy migration and PKV2 integration. MGET reads old/native
values without migration. See
[String page storage and rollout scope](STRING_PAGES.md).


GETBIT/SETBIT and BITCOUNT/BITPOS now operate directly on native String pages,
including BYTE/BIT ranges, key TTL and atomic bit updates. Whole-value bitmap
scans use bounded chunk scratch space. BITOP now supports its eight operators with atomic native page output;
BITFIELD/BITFIELD_RO now support native integer fields and overflow policies.
See [field arithmetic and validation](BITFIELD.md). See [bitmap semantics](BITMAPS.md) and
[BITOP storage and validation](BITOP.md).

## Transaction and command-name checks

MULTI/EXEC/DISCARD and WATCH/UNWATCH now have in-process isolation and
optimistic concurrency checks. PKV3 now replicates whole EXEC groups with one
single-database destination commit. All-path source whole-EXEC durability and cross-DB
recovery remain open; see [EXEC replication](EXEC_REPLICATION.md) and
[transaction behavior](TRANSACTIONS.md). Command names containing NUL
are now consistently rejected as unknown. This intentionally differs from the
local modified=1 Redis reference, which accepts some suffixed names. The earlier
GET-with-NUL shared smoke check was replaced with independent pkv-only checks
for GET and transaction controls; binary key/value coverage remains shared.

Eligible native source EXEC groups now stage and commit once before success
replies. See [source commit routing and remaining fallbacks](SOURCE_EXEC.md).
Cross-database/physical-store and opaque legacy COPY/RENAME paths still need
coordinated recovery or migration.


Legacy expiry/PERSIST changes now use the same native imports as the five typed
DAOs. A successful change emits one bounded root/page batch; old JSON is not
rewritten as a large stored value on page-capable engines with compatible
observers. Failed conditions leave original bytes and WATCH intact. Metadata
changes to already-native keys still read no payload pages. Source EXEC can
stage these legacy operations and commit them together with later commands.
Nonpage engines/observers retain their legacy-compatible path. Legacy
COPY/RENAME now use the imports described below; see also
[expiry migration limits](LEGACY_EXPIRY.md).


Enumerable legacy COPY/RENAME now use bounded destination imports and preserve
source COPY/WATCH semantics. Native commit rejection does not fall back to a
large opaque write. Source EXEC can stage these transfers once; opaque views,
old empty Hash records and incompatible observers retain the compatible path.
See [capabilities and limits](LEGACY_TRANSFERS.md).


LCS now supports its complete RESP2 option set with coherent two-key reads,
compact traceback and read-only legacy access. Fixed transient admission and
the strict NUL-option difference are documented in [LCS](LCS.md).


KEYS now supports complete binary glob matching in the selected logical DB,
including the staged EXEC view and node-local cluster enumeration. SCAN and
WATCH share native metadata inspection. See [keyspace limits](KEYSPACE.md);
full traversal blocks concurrent command admission and is not incremental.


DBSIZE now counts stored roots without decoding values or traversing keys at
query time. Memory/TBB and disk engines support it, including source workspace
deltas and logical DB selection. Disk opens rebuild derived counts from roots;
see [startup cost and count semantics](DBSIZE.md). Active expiry is described
below.


Primary services now reclaim expired keys through a striped TTL index and
locked metadata rechecks. Expiry uses existing native deletion/replication paths,
with bounded candidate quanta, index repair and stop/join lifecycle. Passive
receivers retain source-owned background expiry. See
[active expiry scope, defaults and limits](ACTIVE_EXPIRY.md).


Expiry candidate polling now uses atomic conservative deadline hints to skip
idle stripe locks. All root-deletion decisions still recheck current metadata
under the key lock. Local component measurements and mixed store controls are
recorded in [expiry-index performance](EXPIRY_PERFORMANCE.md); they do not
establish whole-service or stock Redis throughput superiority.


SET now supports IFEQ/IFNE with atomic key-locked comparison, GET and expiry
composition, legacy no-op preservation and bounded native page reads. Failed
conditions preserve WATCH state. Successful source EXEC changes use the existing
native replication batch. See [comparison semantics and tests](SET_COMPARISON.md)
for the documented strict option-order/NUL differences. Digest conditions and
DIGEST/DELEX are now implemented through streaming page reads; see
[digest and conditional deletion](STRING_DIGEST.md).


MSETEX now uses one native key-set transaction for shared expiry, NX/XX and
per-key KEEPTTL. Counted keys participate in cluster/EXEC routing, and source
replication retains native batch boundaries. See [coverage and physical-engine
limits](MSETEX.md); multi-instance disk recovery remains unfinished.


CLIENT ID/GETNAME/SETNAME/SETINFO/HELP and RESET now support connection-owned
metadata, stable unique IDs and queue/WATCH/DB reset. Subcommand arity checks
run before MULTI queuing. CLIENT names and RESET contribute no cluster keys.
See [session semantics and remaining client APIs](CLIENT_STATE.md) and
[HELLO/RESP3](RESP3.md). Default-user authentication is covered in [AUTH](AUTH.md); per-user ACLs
and further client management remain unfinished.


HGETDEL now retrieves/deletes request-ordered Hash fields atomically, preserving
key TTL for remaining fields and removing drained roots. The native AVL delete
traversal reads only requested values and stages one page batch. Legacy JSON
imports, duplicate/null results, type-before-syntax errors, source EXEC and live
replication are covered. See [semantics and limits](HGETDEL.md). Hash field
expiration, HGETEX and HSETEX remain unfinished.


HELLO 2/3 now negotiates typed replies for implemented commands. Reply trees
retain each command's protocol, including HELLO/RESET pipelines and mixed EXEC
children. Maps, sets, numeric scores and ordered pair replies use the verified
RESP3 shapes. [HELLO/RESP3 scope](RESP3.md) explicitly retains default/no-password
user behavior; password/ACL authorization, pushes/tracking and the remaining
server families are unfinished.


AUTH and startup requirepass/masterauth now provide default-user authentication
and protect the companion RPC stream before subscriber registration. RESET
clears login state; HELLO AUTH validates credentials before protocol/name
changes. Wrong-credential and unavailable-source tests also fix closed-stream
reads in receiver reconnects. See [authentication scope](AUTH.md), including the
local reference's missing AUTH-error frame and remaining ACL/TLS work.


A 64-striped public-connection registry now tracks existing CLIENT IDs through
handler destruction. Retained records cannot shut down reused descriptors;
stopping a handler rejects subsequent pipeline commands. Synchronized
metadata snapshots and CLIENT INFO/LIST are now implemented below.
Blocking-command cancellation remains unfinished. See
[connection lifecycle guarantees](CLIENT_REGISTRY.md).


CLIENT KILL now supports ID, TYPE, USER, ADDR, LADDR, MAXAGE and SKIPME,
including cross-thread target shutdown and reply-before-close for self-kill.
Standalone CLIENT commands can run while a different worker owns exclusive KV
admission. Queued KILL stays ordered inside EXEC; PKV preserves complete EXEC
framing despite the local reference's self-kill reply suppression. See
[connection closure scope and reference evidence](CLIENT_KILL.md).


CLIENT INFO/LIST now expose public-connection state and typed RESP3 text, with
ID/type filtering, actual IO/command counters, WATCH/MULTI state and queried
parser/output resources. Allocation gauges describe PKV representations and
known-owned memory, rather than copying Redis allocator values. See
[scope and limits](CLIENT_INFO.md). The earlier registry-only statements are
superseded by these implementations; broader Redis compatibility remains open.


Named-user ACL authorization is now implemented for the public command path:
AUTH/HELLO bind stable identities, SETUSER publishes complete staged rules,
queue and EXEC checks enforce command/key permissions, and DELUSER closes old
sessions without reviving them when a username is reused. Rules include read/
write key modes and independent selectors. This supersedes earlier statements
that category metadata or the default-user AUTH layer had no runtime ACLs.
Startup aclfile and ACL SAVE/LOAD now persist the complete user table with
staged replacement, retained-user sessions and removed-user closure. ACL LOG
now records recent public authentication and command/key failures, with bounded
retention, aggregation, reset and RESP3 maps. The companion RPC keeps its
separate startup credentials. See
[ACL scope](ACL_PERMISSIONS.md) and [persistence](ACL_PERSISTENCE.md).

See [security event scope and validation](ACL_AUDIT.md).


CONFIG GET/SET/HELP now provide real values for implemented configuration
features, glob/name matching, atomic password/audit-limit changes and explicit
errors for unsupported Redis persistence activation. This supersedes earlier
fixed save/appendonly replies and statements that CONFIG SET is unavailable.
The full Redis configuration inventory is not implemented. See
[configuration semantics](CONFIGURATION.md).


Local Pub/Sub now implements SUBSCRIBE/UNSUBSCRIBE, PSUBSCRIBE/PUNSUBSCRIBE,
SSUBSCRIBE/SUNSUBSCRIBE, PUBLISH/SPUBLISH and six PUBSUB inspection children.
Delivery spans worker threads and logical databases, with RESP3 push frames,
subscription ACLs, live-policy revalidation and bounded subscribed output.
CLIENT type filters and INFO report actual subscriptions. This supersedes the
earlier lack of a public Pub/Sub runtime. Cross-node cluster/replication
propagation, configurable output-limit policies, Streams and scripting remain
pending. See [semantics and verification](PUBSUB.md).

The Pub/Sub integration [command inventory](
benchmarks/pubsub-command-inventory-2026-11-01.json) identified 228 PKV names
versus 412 local-reference names. [That revision's validation](
benchmarks/pubsub-validation-2026-11-01.json) records its eight-configuration
1,056-test matrix and matched-protocol GET measurements.

[RANDOMKEY and TIME](KEY_SAMPLING.md) now provide key-only root sampling and
cached microsecond EXEC time. The [current inventory](
benchmarks/dense-command-inventory-2026-11-01.json) identifies 230 PKV names
versus 412 reference names, including subcommands: 183 reference names remain
missing and cluster|addslave is an extra alias. Presence does not establish
semantic parity. [Dense-index validation](
benchmarks/dense-validation-2026-11-01.json) records 1,086 passing CTests
across eight configurations, unchanged codecs and matched GET/write measurements.
[Dense memory root indexes](DENSE_KEY_INDEX.md) now eliminate the selected
collision-bucket walk, with explicit memory/rollback tradeoffs. Disk/workspace
sampling remains linear. Faster disk rank selection, atomic cross-DB MOVE,
Streams, scripting and standard replication remain work toward the complete
Redis goal.


[Prepared page commits](PREPARED_PAGE_COMMITS.md) now coordinate reversible
MDB/oneTBB preparations before publishing either database. This internal
foundation does not add a public MOVE command: independent disk commit domains
and cross-DB replication grouping still require coordination. Its
[validation](benchmarks/pair-validation-2026-11-01.json) records 1,094 passing
CTests across eight configurations and 25 additional focused repetitions.
The [current name inventory](
benchmarks/pair-command-inventory-2026-11-01.json) remains 230/412 with 183
missing names, including MOVE. No complete Redis compatibility claim is made.


[Native commit buffers](NATIVE_COMMIT_BUFFERS.md) now reserve MDB scratch data
only on first use. Existing-root native overwrite/erase allocation counts fell
from 6/5 to 4/2 in the isolated probe, with unchanged atomic rollback semantics.
The [latest validation](benchmarks/lazy-validation-2026-11-01.json) records
1,102 passing CTests and 30 focused repetitions on 574 frozen source files.
The [latest inventory](benchmarks/lazy-command-inventory-2026-11-01.json) still
reports 230/412 names and 183 missing, including MOVE. No new public command,
disk cross-engine transaction or full Redis compatibility is claimed.


[Physical WAL barriers](DURABILITY_BARRIERS.md) now provide an internal disk
fence without changing command registration, page layouts or ordinary writes.
The [latest validation](benchmarks/sync-validation-2026-11-01.json) records
1,118 passing CTests and 47 further repetitions, including acknowledged WAL
write/delete process-kill recovery and partial-initialization rejection.
The [latest inventory](benchmarks/sync-command-inventory-2026-11-01.json)
remains 230/412 with 183 missing names. This is not a Redis SYNC command,
cross-engine commit or public MOVE implementation.


The [internal MOVE decision journal](MOVE_RECOVERY.md) passed native IO,
allocation and process-kill checks in the
[latest 1,134-test matrix](benchmarks/journal-validation-2026-11-01.json),
plus 40 focused repetitions. The online server coordinator and grouped
replication remain unfinished, so public MOVE remains missing. Existing
command registration and advertised name counts are unchanged; the prior
230/412 inventory with 183 missing names is retained as reference evidence.


[Native MOVE redo](NATIVE_MOVE_REDO.md) now applies existing internal decisions
across native disk DBs with one bounded private page per commit. Real String,
Hash, List, Set and ZSet payloads pass native replay checks. A native WT probe
also corrected positioned getter failures being misclassified as absence/end.
[Recorded checks](benchmarks/redo-validation-2026-11-01.json) comprise an initial
298-test targeted selection and 84 WT corrective checks with separate source
states. Public MOVE, server policy/effects integration, grouped replay and
O(1) volatile transfer remain outstanding. Command registration remains unchanged.

Optional [bound startup recovery](MOVE_STARTUP.md) now validates the native
storage domain and replays existing decisions before clients, expiry and RPC
workers run. Persistent root markers reject changed/disabled configurations;
binding happens only after native DB ownership succeeds. The
[startup checks](benchmarks/startup-validation-2026-11-01.json) cover interrupted
replay, rejected configurations and competing server startup. They add no public
command or complete Redis compatibility claim.


The internal [native MOVE storage coordinator](MOVE_STORAGE.md) now retains
admission and both key locks through preparation, source WAL synchronization,
decision publication and physical completion. Startup shares its bounded
application code. A reproduced callback/key-alias bug was corrected by owning
the key before locking and using that snapshot throughout. The server policy,
logical effects, grouped replication and public MOVE command remain unfinished;
command and codec sources retain their prior bytes. See the
[scoped coordinator checks](benchmarks/coordinator-validation-2026-11-01.json).


[Memory page-family handoff](MEMORY_MOVE_FAMILIES.md) now transfers MDB/oneTBB
ownership without per-page copying, preserving byte/address identity and local
WATCH, expiry and readiness effects. It explicitly declines unsupported pairs
and does not emit incomplete root-only observer events. This remains an internal
primitive: public MOVE and grouped observer/replay integration are unfinished.
See [allocation and regression evidence](benchmarks/family-validation-2026-11-01.json).


[Memory cross-DB replay](CROSS_DB_REPLAY.md) now prepares all received native frames
before publishing any. The private v3 receiver supports MDB/oneTBB groups;
public MOVE, its source publication and durable disk groups remain
unintegrated. Single-DB replay retains its existing path.


[Replica read retention](REPLICA_READ_RETENTION.md) prevents expired client reads from
erasing pages needed by delayed primary deltas. Logical reads still report
missing data; authoritative replay and explicit writes keep their native paths.
