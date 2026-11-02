# Protocol regression tests

Run independently of the full server. Requires a C++ compiler, CMake, ACL headers
and `libacl_all`, zlib, and platform system libraries. Storage engines and yyjson
are not required.

```sh
cmake -S tests -B /tmp/pkv-protocol-tests -DPKV_TEST_SANITIZERS=ON
cmake --build /tmp/pkv-protocol-tests -j 4
ctest --test-dir /tmp/pkv-protocol-tests --output-on-failure
```

For an ACL installation outside the default search paths, add
`-DCMAKE_PREFIX_PATH=/path/to/acl/prefix`. Sanitizers require Clang or GCC.
The tests run with both C++11 and C++17, and both object cache implementations.
Checks use explicit failures and remain enabled in Release builds.

Coverage: RESP2 round trips across chunk sizes, mixed pipelines, partial frames
surviving `clear()`, embedded NUL/CR/LF, empty and null strings/arrays, integer
bounds, malformed numbers/terminators, resource limits, wide arrays, and early
connection teardown with partially parsed nested objects.

The parser currently limits bulk strings to 512 MiB, arrays to 1,048,576 elements,
line content to 64 KiB, and nested children to depth 128 (root depth is zero).
These are implementation limits rather than a claim of complete Redis parity.
Inline parsing retains the existing ACL tokenization; Redis inline quoting and
RESP3 are not covered by this suite.

## Full memory server and storage tests

From the repository root, with ACL installed and an existing yyjson checkout:

```sh
cmake -S . -B /tmp/pkv-memory-tests -DCMAKE_BUILD_TYPE=Debug \
  -DPKV_YYJSON_SOURCE_DIR=/path/to/yyjson -DPKV_TEST_SANITIZERS=ON
cmake --build /tmp/pkv-memory-tests -j 4
ctest --test-dir /tmp/pkv-memory-tests --output-on-failure
```

Use an installed yyjson by omitting `PKV_YYJSON_SOURCE_DIR`. Optional engine flags
remain off unless explicitly enabled. The full suite adds storage fixture tests
and Python 3 socket tests for the real two-thread memory server. Socket tests use
random loopback client/RPC ports, temporary configs and data, and clean up their
own processes. They require local TCP listener permissions. Disable them with
`-DPKV_SOCKET_TESTS=OFF` in environments without those permissions.

To execute the same socket checks against a reference Redis build as well, set
`-DPKV_REFERENCE_REDIS=/absolute/path/to/redis-server` during configuration. This
runs explicit expected-result checks against both servers; it is not an exhaustive
Redis differential suite. You can also invoke it directly:

```sh
python3 tests/integration_test.py --pkv /tmp/pkv-memory-tests/pkv \
  --redis /absolute/path/to/redis-server
```

Socket coverage includes basic SET/GET, empty and binary keys/values (all byte
values and deterministic randomized payloads), PING/ECHO, command arity, unknown
commands, wrong-type GET, ordered pipeline errors, byte-wise fragmented requests,
QUIT ordering/connection closure, and eight concurrent clients on separate keys.
Storage fixtures check legacy JSON records, corruption retention, DAO reuse,
expiry rewriting of binary values, write failures, and the built-in map's binary
storage. The suite now also checks Hash binary values, multi-field counts, duplicate
arguments, HMGET ordering, nil placeholders, wrong-type retention, hash TTL
preservation, last-field deletion, TYPE, DEL counts, 64-bit EXPIRE validation, and
non-positive expiry deletion. Eight clients also add 400 independent fields to a
shared hash. Storage tests exercise 4 OS threads updating one hash, and 4 threads
running 32 fibers that yield while holding key locks for 640 atomic counter
updates. They verify callback-exception lock release and failed I/O propagation.
Multi-key isolation and disk backends need further tests.


Record-codec tests also run during a full build. They check v0/v1 round trips,
all byte values, exact millisecond deadlines up to the signed 64-bit boundary,
legacy export precision rejection, duplicate/invalid metadata, future-version
preservation, legacy mutation to v1, and a DB-bound non-JSON codec used through
the same DAOs. The storage format and rollback policy are in
`doc/STORAGE_FORMAT.md`.

The socket suite additionally covers SET NX/XX/GET/KEEPTTL and
EX/PX/EXAT/PXAT, exact PEXPIRETIME preservation through hash mutation,
PEXPIRE/EXPIREAT/PEXPIREAT, PTTL, EXPIRETIME, PERSIST, and expiry conditions.
It runs 20 NX races across eight clients and checks a chain of 200 atomic
SET GET exchanges. Current expected-result coverage totals 6,525 assertions per
server; this still covers only a subset of Redis.

## Codec microbenchmark

Use a Release build without sanitizers. Build with `-DPKV_BUILD_BENCHMARKS=ON`,
then run `build/tests/codec_bench 100000` while other builds/tests are idle.
The CSV output compares JSON v0/v1 encode and decode-plus-read throughput for
32-byte and 1-KiB strings and 8/64-field hashes. Each case has warmup work and five
samples, with format order alternating. Compare medians and record sizes.
This measures codec calls only, without TCP, locks, storage I/O, expiry processing,
or whole-server command handling; it cannot establish server throughput or a
performance advantage over Redis.


The full suite now also includes native batch allocation-failure injection and
cluster routing tests (338 additional assertions per server). Multi-key socket
coverage checks MGET ordering and nils, MSET last-value handling, MSETNX rejection
without partial writes, TTL clearing, EXISTS duplicate counts, coherent concurrent
MSET/MGET snapshots, MSET/DEL snapshots, and overlapping two-key MSETNX races.
Storage tests exercise yielded two-key transfers, failed commit/read preflight,
callback abort/exception handling, and lock-aware replication apply without
observer echo. See `doc/ATOMIC_BATCH.md` for supported engines and limitations.

`batch_failure_test` overrides allocation only in its own test process, with the
failure budget activated just before native commit. It tries each allocation
point until a complete commit succeeds and verifies all old/new/deleted keys
after each failed commit. It does not add fault-injection switches to production.

Cluster tests start separate temporary cluster-mode nodes without assigned slots.
They compare CLUSTER KEYSLOT for fixed and randomized binary keys against an
independent CRC16 implementation and the reference Redis binary, then check
CROSSSLOT and CLUSTERDOWN replies. They do not test topology convergence, MOVED,
ASK, migration, or failover. Run just these tests with
`ctest --test-dir build -R cluster_routing --output-on-failure`.

## Atomic string command regression

The ordinary socket suite covers INCR/DECR/INCRBY/DECRBY, APPEND, STRLEN,
GETRANGE/SUBSTR, SETRANGE, GETDEL, GETEX, GETSET, SETNX, and SETEX/PSETEX.
It checks signed integer boundaries, canonical stored integers, TTL retention,
binary range endpoints including signed index extremes, zero padding, growth
rejection before allocation, empty SETRANGE without key creation, argument/error
ordering, and exact expiry timestamps. Eight shared-key clients verify 800 INCR
results, APPEND positions and complete blocks, non-overlapping SETRANGE updates,
and a single GETDEL winner. Storage fixtures additionally inject failed reads
and writes and verify old encoded bytes and TTLs remain intact. Codec tests
exercise length/range access and mutations through an independent non-JSON codec.

The 6,525 common assertions run on each server. Eight separate pkv-only assertions
check relative expiry overflow rejection and preservation of existing values and
TTLs. The local reference Redis 8.6.2 build accepts the maximum signed 64-bit
PSETEX duration; pkv rejects the overflowing deadline deliberately. See
`doc/COMPATIBILITY.md` for this difference. No 512-MiB allocation is required by
the growth-limit tests, and no full-server throughput claim follows from them.

## Hash conditional updates and selective reads

Socket tests cover HSETNX, HINCRBY, HLEN, HSTRLEN, HEXISTS, HKEYS, and HVALS:
missing keys/fields, empty versus absent fields, all byte values, integer limits,
non-canonical integers, wrong-type and argument-error precedence, expiry
recreation, exact key TTL retention, and unordered key/value enumeration.
Eight clients share one counter (800 increments) and compete on 20 HSETNX
fields. Storage failure fixtures check that failed reads/writes and integer
overflow retain the original encoded bytes and TTL. Record-codec tests cover
JSON size/field-length access and a selective read-only view which fails if HMGET
or field-length fallback attempts full hash materialization. These tests cover
key expiry; hash field expiry is not implemented.

## List storage and command regression

Each ordinary server run additionally performs 2,002 List assertions, reported
separately from the 6,525 string/hash/key assertions. LPUSH/RPUSH,
LPUSHX/RPUSHX, LPOP/RPOP with optional count, LLEN, LRANGE, LINDEX, LSET and
LTRIM cover missing/expired keys, binary and empty elements, duplicate ordering,
negative indexes and signed extremes, exact TTL retention, key deletion after
draining, wrong types and argument-error precedence. Raw RESP checks distinguish
nil arrays from nil bulk strings. A deterministic 300-step model checks contents,
length and type after every mutation. Eight clients push 320 two-element blocks,
verify return lengths identify each complete block, and consume all 640 items
exactly once from both ends. Cluster routing now checks the eleven new commands
as well, for 338 total routing assertions per server.

Fourteen separate pkv-only assertions verify extreme LSET indexes do not wrap,
mutate a different element or change TTL. The local Redis reference narrows these
indexes internally; see `doc/COMPATIBILITY.md` for the deliberate difference.
Codec tests verify owning List views, default view accessors, binary payloads,
exact expiry boundaries, invalid/empty arrays, and rejection of legacy export and
unsupported codecs. Storage fixtures verify failed reads, writes, deletion and
future/corrupt records leave original values and TTLs intact. Blocking operations,
waiters and long-running fault/performance tests are not covered by this milestone.

## List multi-key transactions

An additional suite runs 2,963 assertions on each server for LMOVE, RPOPLPUSH
and LMPOP. It checks both ends for source/destination, same-key rotation, binary
and empty items, missing/expired keys, unchanged source after a wrong-type target,
existing/new destination expiry, nested replies, duplicate keys, declared-key
ordering, count/numkeys bounds and syntax precedence. A 240-step model verifies
contents and types after every action. Eight clients move 480 unique values to
three destinations, then perform 480 opposing moves; all values remain present
exactly once. They also drain 480 other values with LMPOP without duplication or
loss. Cluster tests add four cross-slot and six same-slot cases for these commands.

Storage fixtures inject a failed native batch, a second-record encoding failure,
failed preflight reads, unsupported multi-change backends, and failed LMPOP
write/delete operations. They verify unchanged encoded records and TTLs, cleared
results on failure, same-key no-op behavior, and deferred cleanup of unselected
expired records. This is in-process atomicity on the tested memory engine; it
cannot establish crash durability or transactional replica replay.

## List editing and position search

LINSERT, LREM and LPOS add 2,710 assertions per server. With Set coverage,
ordinary common coverage totals 31,936 assertions, and the cluster suite contains
338 assertions per server. Existing 8 expiry overflow and 14 LSET index
safety assertions remain independent pkv-only checks, not Redis comparisons.

Search tests cross binary/empty/numeric-looking values, positive/negative/extreme
ranks, COUNT zero/one/large, MAXLEN zero/bounded/large and option repetition. They
check nil versus integer versus array replies and argument errors before lookup,
including explicit signed-minimum rejection. Mutation checks verify first-pivot
insertion, bounded removals from either end, stable retained order, key deletion
when empty and TTL retention. A deterministic 280-step model checks content,
length and type after each action. Eight clients insert 240 distinct elements
before one pivot and remove 128 duplicate matches exactly once.

Storage fixtures inject failed reads/writes/deletion and check unchanged encoded
bytes/TTLs, no-op edits without writes, LPOS without rewriting, and corrupt record
retention. Codec tests exercise the default search fallback and JSON direct search,
and bind a selective view that rejects full List materialization. LPOS must remain
usable through that view. Tests validate behavior and copy/traversal boundaries;
no complete-server performance advantage or constant-time JSON parsing is implied.

## Set members, moves and algebra

A separate 3,257-assertion Set group runs on each server. SADD, SREM, SCARD,
SISMEMBER, SMISMEMBER, SMEMBERS, SMOVE, SUNION/SINTER/SDIFF and STORE variants
cover missing/expired keys, binary/empty/numeric-looking members, duplicates,
wrong types, arity, exact TTL retention, overwrite/empty-result deletion and
source/destination aliasing. Results are compared as unordered unique members;
SMISMEMBER replies preserve argument order and duplicates. Every algebra source
must be valid even if an early missing source already makes the result empty.

A deterministic 280-step model checks contents and type after every mutation or
algebra operation. Eight clients add 320 distinct and 16 shared members, then
compete to remove the shared members. They execute 640 two-way SMOVE operations
while another client repeatedly checks coherent SUNION and SINTER snapshots:
all 320 members remain in the union and none appear in both sets. Cluster tests
add seven cross-slot and thirteen same-slot cases for the new commands.

Codec fixtures validate owning views after input destruction, default and selective
membership accessors, deadline extremes, invalid/duplicate payloads and unsupported
legacy export/custom codecs. Storage fixtures cover failed I/O, deletion, native
batch commit and second-record encoding, plus unsupported multi-change engines,
single-change moves into an existing member, no-op writes, exact TTL retention
and deferred expiry cleanup. These validate in-process memory behavior; Durability, replica transaction boundaries and long-running performance remain
unverified or unimplemented.

## Set random sampling and intersection cardinality

A further 728 common assertions per server cover SPOP, SRANDMEMBER and
SINTERCARD, bringing ordinary common coverage to 31,936 assertions and cluster
coverage to 338 assertions per server. Random results are validated by membership,
size, positive-count uniqueness, negative-count replacement, exact remaining
contents and TTLs. No equality of random draws across implementations is assumed.
Tests include missing/singleton/empty-byte members, zero and large clamped counts,
signed bounds, error/lookup precedence, COUNT versus scalar replies, LIMIT and
repeated options. A 120-step model checks count-only intersections; eight clients
consume 480 SPOP members without duplication or loss.

Fixed-seed helper tests exercise all six two-member subsets of four members and
20,000 replacement draws with broad frequency bounds, plus empty populations,
unique-count clamping and impossible allocation sizes. These check regressions;
they do not certify randomness or cryptographic security. Storage fixtures inject
failed reads/writes/deletion, check cleared SPOP failure results, TTL retention,
no writes for zero count/SRANDMEMBER/SINTERCARD, corruption handling and deferred
expiry cleanup. Default codec membership fallback is also checked.

Six additional pkv-only checks reject a replacement sample larger than the
response vector can represent. They verify the error, preserved data/TTL and a
subsequent PING; the giant request is never sent to Redis. These join the existing
8 expiry-overflow and 14 LSET-index checks and remain separate from common
compatibility counts. Whole-server memory budgets and exhaustive OOM recovery
are still incomplete.

## Stateless scanning

The socket suite adds 141 SCAN/HSCAN/SSCAN and 70 ZSCAN scenarios per server,
reported separately from
fixed reply assertions because the number of pages is implementation-specific.
SCAN checks 180 matching keys and TYPE filters. HSCAN/SSCAN cover 20 binary glob
patterns, empty names, NOVALUES, option/cursor bounds, continued connections and
cursor reuse across newly opened connections. Mutating transient fields/members
between pages must not omit 220 continuously present entries. Cluster tests now
total 338 assertions, including local SCAN and keyed HSCAN/SSCAN/ZSCAN routing.

Codec tests add a 1,000-case independent wildcard DP oracle, deep nonrecursive
patterns, full unsigned cursor parsing and default scan accessors. Storage tests
exercise failed enumeration/read, unsupported backends, expiry filtering,
duplicate-field corruption, unchanged collection bytes and stable keys during
1,000 concurrent native insert/delete operations. Eight independent pkv-only checks
reject NUL-suffixed cursors and verify the connection stays usable; that differs
from the local Redis reference's truncation and is not counted as parity.

ZSCAN scenarios cover empty/binary members, exact score strings, member-only
MATCH, repeated options, malformed counts/cursors, NOVALUES rejection, lookup
and option-error precedence, TTLs, portable/interleaved cursors and continuously
present members during insert/delete/score updates. Codec fixtures check selective
and fallback scans, partial failure cleanup and concurrent owning-view access.
See `doc/SCAN.md` for cursor ownership and performance limits. Native pages copy stable sub-buckets rather than whole shards; collection scans
parse whole records, COUNT is a hint, and scans
are not global snapshots. Disk/backend scanning and bounded per-call work remain
unfinished.

## Native scan bucket regression and measurement

The native memory layout now uses 256 lazy buckets per logical shard without a
second full key index. Storage fixtures put 4,096 keys in one logical shard and
require COUNT 1 to paginate below 256 keys per page for that fixture, cover every
key exactly once, drain empty buckets correctly and permit overwrite/reuse.
Existing concurrent mutation, transfer and snapshot suites run unchanged.
Allocation-failure injection now checks 51 native batch and 15 scan-page failure
points on this machine; those counts are allocator/data dependent. Scan failures
must clear partial keys/cursors and leave the database usable and unchanged.

`scan_bench.py` compares retained before/after Release binaries with temporary
loopback servers, alternating order over five rounds. It checks complete steady
iterations and reports page latency, page sizes, full traversal costs and short
pipelined SET/GET controls. Run it with other jobs idle; results are not assertions
or Redis throughput comparisons. See `doc/SCAN.md` and its raw benchmark data for
the measured tradeoff: smaller page requests, more round trips, slower complete
traversals at the same small hint, and added pointer/map memory.

## Sorted Set foundation

The basic Sorted Set group has 2,259 assertions and its range group adds 2,458
assertions on each server; sampling adds 908 and multi-pop adds 2,268 further
assertions; algebra adds 1,821 more, bringing common
command checks to 31,936 plus 211 separately counted scan scenarios. Cluster
routing now totals 338 assertions per server. Basic coverage includes twelve commands,
ZADD condition/CH/INCR flags, ordered repeated members, negative ranks, binary
ties, nil/array replies, canonical score strings, infinity/subnormals/hex input,
invalid-float preflight, NaN arithmetic, TTL retention and generic key integration.
A 200-step model checks contents and types; eight clients verify 800 increment
scores, 240 independent added members and 320 members popped exactly once.
Compact Redis negative-zero scores can be canonicalized to 0; that scenario
explicitly checks zero semantics, while a noncompact case checks retained -0.

Codec tests add 10,000 bit-exact non-NaN double text round trips, v1 score payload
validation, custom view fallbacks, rejected legacy/custom-codec writes and owning
index checks. Four threads repeatedly read one shared 250-member view during
lazy score-order publication. Numeric parsing/formatting is checked under
fr_FR.UTF-8 when available and the process locale is restored. Storage fixtures
cover failed lookup/write/delete, no-op writes, cleared pop failures, NaN score
arithmetic, unchanged TTL/payloads and corruption/future-record retention.
The range group covers modern and legacy score/lex ranges, open/closed/infinite
bounds, empty/binary members, both directions, extreme LIMIT values, count and
removal commands, range-store TTL/overwrite/alias/expiry behavior and concurrent
snapshot stores. Codec fixtures compare indexed and fallback selection/count
windows; storage fixtures inject failed read/write/delete/encoding and preserve
future records. ZRANGESTORE has explicit same-slot and CROSSSLOT routing checks.
See `doc/SORTED_SETS.md` for lex preconditions and remaining commands;
these tests do not establish complete Sorted Set parity or Redis performance.

## Sorted Set random selection

ZRANDMEMBER adds 908 assertions per endpoint, covering scalar nil/empty/binary
members, unique/clamped positive counts, exact negative-count lengths, optional
flat WITHSCORES replies, score association, 300-member noncompact populations,
wrong types, arity/options, signed count extremes, precedence of count/options
before lookup and TTL/content retention. Five clients concurrently sample and
replace a key with disjoint populations; each sampled reply must belong entirely
to one population and retain its scores. Membership and uniqueness properties are
compared rather than expecting identical random replies from different servers.

Ten independent pkv-only checks reject replacement counts that cannot fit the
result vector and verify score, TTL and PING afterward. Those requests are never
sent to the reference process. Parser-level WITHSCORES count overflow limits are
checked on both endpoints, including absent keys. These are representability
limits, not a configurable response budget.

Codec fixtures require selective sampling without full map materialization,
exercise the custom-codec fallback and clear partial accessor failures. The shared
Set/Sorted Set helper preserves existing fixed-seed subset/frequency checks and
compares 1,000 selections for matching member indexes and associated scores.
Storage fixtures cover failed lookup, future records, unchanged live bytes,
missing-result cleanup and signed/oversized counts. Production memory/CPU limits,
very-large-key sampling latency and cryptographic randomness are not established.

## Sorted Set multi-key pop

ZMPOP adds 2,268 assertions per endpoint. Coverage includes request-ordered
selection, MIN/MAX score and binary-member tie order, default/large COUNT,
nonspanning pops, repeated keys, missing/expired candidates, early wrong types
and irrelevant later wrong types, TTL retention, final-key deletion, nested
RESP2 pair arrays and an explicit raw nil-array frame. Parser tests validate
numkeys/count signed bounds and error ordering before lookup.

A 260-step model checks all members/scores/deadlines after mixed mutations and
multi-key pops. Eight clients with overlapping/reversed/duplicate key lists pop
all 640 initial members exactly once and leave both keys absent. Cluster checks
add two cross-slot and two same-slot/unassigned-slot cases, for 338 total routing
assertions. Storage fixtures test a legacy single-write backend, failed read,
write, delete and encode, cleared failed outputs, priority preservation,
corruption before/after the selected key, and unchanged nonselected keys/TTLs.
These checks establish the tested local memory synchronization behavior, not
disk crash consistency, replication failover or bounded large-key latency.

## Sorted Set algebra and cardinality

ZUNION/ZINTER/ZDIFF, their STORE variants and ZINTERCARD add 1,821 common
assertions per endpoint. Tests cover mixed Sets/Sorted Sets, binary and
integer-like members, repeated weighted sources, decimal/hex/infinite weights,
SUM/MIN/MAX, repeated options, sorted replies, source aliases, destination
type/TTL replacement, empty deletion, all-source validation before option errors,
LIMIT extremes and derived NaN/infinity rules. A 160-step model checks weighted
results/stores/cardinality, while five clients probe and store duplicate-source
snapshots during concurrent increments. Seventeen new routing cases cover all
commands and separate destination/source slot checks, for 324 routing assertions.

When a Redis reference is configured, a separate paired test runs 600 live
ZUNION/ZINTER comparisons over 300 equal-cardinality cancellation inputs, up to
35 sources with scores including +/-1e16 and +/-1e300. Source tie ordering is the
host qsort behavior, which can affect nonassociative floating sums; a stable_sort
implementation failed 70 cases before correction. Reply bytes must match, apart
from the already disclosed compact signed-zero case. These comparisons are
reported separately from per-endpoint totals and do not prove cross-platform
floating ordering.

Codec fixtures exercise selective source visitors/single-member score lookups
without full map materialization, materializing fallbacks, successful early stop,
partial traversal/lookup failures and SINTERCARD candidate-copy avoidance.
Storage fixtures validate legacy one-write stores, unchanged sources, read/write/
delete/encode failure, option callback abort, wrong-type/corrupt inputs before
early exit, cleared failed result/count and rejected direct NaN weights.
No result map is built for ZINTERCARD; JSON still decodes every source record.
These checks do not establish production memory/latency budgets or disk/replica
crash consistency.

## Key rename

RENAME/RENAMENX add 2,696 assertions per endpoint. Coverage includes all five
supported value kinds, exact source TTL transfer, persistent-source overwrite,
same/missing/expired keys, blocked NX, destination expiry, binary/empty key names,
arity and a 200-step seven-key value/expiry model. Twelve eight-client NX races
have one successful move and seven no-source errors. Five clients perform/read
120 token moves; every MGET snapshot must contain exactly one token. Six routing
checks cover cross-slot, same-key and same-slot distinct-name requests.

Storage tests reject non-atomic backends without partial changes, inject failed
lookup/commit, preserve legacy v0 bytes/expiry and reject corrupt/future sources,
including malformed/duplicate Hash fields on same-key requests. Codec fixtures
rename through selective read-only Hash and independent text codecs, ensuring no
encoding is required. Full source validation has an optional accessor and custom
materializing fallback. See `doc/KEY_TRANSFERS.md` for costs and the outstanding
remaining MOVE/replication/database-lifecycle requirements; physical db_count is not a logical
DB namespace. Current common command coverage is 31,936 assertions per endpoint,
plus 211 scan scenarios and 600 paired floating comparisons when configured;
cluster routing totals 338 assertions. Disk/replica atomic rename remains unproven.

## Logical databases and COPY

The new ordinary group adds 1,341 assertions per endpoint, bringing ordinary
command coverage to 31,936, with a further nine configured-database checks on a
separate service. SELECT checks connection-local selection, errors without state
changes, pipelines and default/last valid DB indexes. COPY checks all five value
kinds, DB/REPLACE/repeated options, source/target expiry, persistent replacement,
same-name cross-DB copies, same-object errors, source independence and six clients
copying in opposite directions. Every data family and SCAN is isolated by DB.
The configured service uses databases=3; db_count remains 16 physical shards.
Eight cluster additions check SELECT's DB 0 rule and COPY key routing, for 338
routing assertions per endpoint.

Storage/codec fixtures cover one shared lazily created DB under concurrent access,
failed creation/retry, lookup/write/encoding failure, exact source preservation,
text/JSON conversion with expiry and read-only codec copy. Logical DB instance
lifetime is held by connections. Disk reopen and DB N replication/recovery are
not validated: legacy peers continue to serve DB 0 only, while negotiated version 1 now
propagates database identity for live updates. See `doc/LOGICAL_DATABASES.md` for the full scope and remaining work.

## Replication transport and live stream

RPC and queue tests bring the tested Unix matrix to twenty-four tests.
`rpc_transport` checks single/batched binary and empty keys/values, maximum frame
lengths, fragmentation, truncated input, invalid operations/reserved fields,
DEL payload rejection, stale-output cleanup, capacity validation and no prefix
from rejected batches. A failed subscriber flush with queued messages exercises
exactly-once release and post-close rejection under ASan/UBSan.

`replication_stream` adds 420 independent pkv assertions. Actual RPC subscribers
must both register while idle, receive empty/binary SET/DEL events with exact
expiry and no GET traffic, and retain order for 400 same-key increments from four
writers during 25 connection resets. Nonzero DB events must remain out of the
legacy DB 0 stream. These are separate from ordinary command and local Redis
reference comparisons; no custom RPC/Redis replication parity is claimed.
See `doc/REPLICATION.md` for frame limits, native byte order, remaining version/
DB negotiation, full sync, replay, transactional visibility and queue budgets.

## Negotiated multi-DB replication

The same RPC/stream test entries now cover version 1 as well. Native checks use
exact network-order header bytes, reject malformed controls/lengths/DB indexes,
require READY before DB N apply and reject duplicate READY. Observer fixtures
check database identity, retained dispatcher lifetime, no apply echo and failed
write/delete behavior; replaying an absent deletion succeeds without masking I/O
failure.

Live tests add 18 upgrade assertions (legacy filtering plus DB 0/1/7/15 frames),
12 assertions against a second actual pkv process receiving via replica_rpc_source,
and eight assertions against reconnecting/fake legacy upstreams. They check
same-name DB isolation, TTLs, typed updates, cross-DB COPY/deletion, reset of
negotiation state and legacy DB 0 fallback. The original 420 legacy assertions
remain. No initial synchronization, replay or replica transaction guarantees are
implied. See `doc/REPLICATION.md` for configuration and remaining requirements.

## Replication queue budgets

`replica_queue_limits` adds 1,521 independent checks for byte/count limits,
READY accounting, overflow disconnection and healthy-peer progress during 1,500
writes with a stalled peer. Native transport fixtures check exact arithmetic
boundaries, in-flight charges, blocked-send shutdown, draining and publication
epochs. These checks are independent of the ordinary Redis comparison totals.
See `doc/REPLICATION.md` for settings, accounting and recovery limitations.

## Bounded List page format

`list_page_codec` covers fixed metadata, 16 KiB pages, fragmented large members,
network-order bytes, structural validation, truncated/corrupted input and
allocation-failure output preservation. See `doc/LIST_PAGES.md` for the format
and native command integration. Command-level bounded storage is checked by
`paged_list_dao` and the live subscriber/receiver fixtures.

## Atomic page storage

`page_storage` checks the independent native page namespace, bounded writes,
read-own-changes, ordinary/replica cleanup, aborts, backend failures, allocation
rollback and 800 multi-thread/fiber root/page increments. A 1 MiB List member is
stored across bounded pages with a 40-byte root. One-page allocation remains
bounded for owners with 10 versus 5,000 stored pages. Observed writes require page-capable dispatchers; the live replication observer
now publishes atomic page events. List commands now use the page-aware DAO. See `doc/PAGE_TRANSACTIONS.md` for remaining work.
The unsupported-engine fixture also covers the AVL backend's corrected ownership
of database tables during destruction.

## Page-transaction stream components

`page_replication` tests a provisional PKV2 stream over actual socketpairs,
including exact bytes, bounded frames, truncated/invalid sequences, owning
capture, allocation failure and atomic native apply without observer echo. A
transaction of 80 bounded List pages exceeds the old single-value RPC limit;
its 40-byte root/expiry and DB 15 isolation survive native application. Live
negotiation, publication queues and receiver-loop application are now covered by
`live_page_replication` and `page_live_stream`.
See `doc/PAGE_REPLICATION.md`; List commands now publish bounded pages.

## Live PKV2 publication and receiver

`live_page_replication` inspects actual source/destination native pages across
observer publication, subscription negotiation, queues and packet application.
It checks per-frame charging, byte/count overflow at both stages, DB N legacy
filtering, v1 disconnection and an ordinary update following a complete group.
`page_live_stream` runs a real pkv process against a TCP upstream, exercising
commit visibility, reset/discard, invalid interleaving, missing base roots,
v1 fallback, DB isolation and v2 subscriber requests. These are custom replication
checks; full synchronization remains pending. The same suite now inspects
actual List command frames, including a root-only TTL update.

## Packed List storage operations

`list_store` exercises native packed pushes/pops, range/index, trim, streamed
position comparison, metadata-only expiry and atomic replacement. It checks
10000 small members, 400 deque-model operations, a 1 MiB member, endpoint page
access counts, malformed chains and id exhaustion. Allocation injection verifies
replacement rollback, including detached old page maps. Four threads each run
four yielding fibers. Source/receiver fixtures verify ROOT_REPLACE propagates
actual List-store replacement and reclaims old pages. See `doc/LIST_STORAGE.md`;
ordinary List commands now use the page-aware DAO.

## Bounded List middle edits and copies

`list_edits` tests replacement, insertion, signed-count removal, same/cross-key
moves, independent cloning and coordinated cross-DB page copies. Its 500-step
vector model includes empty/binary and fragmented values. Locality/allocator
checks reject whole-List/member materialization during 1 MiB middle edits and
cloning. It tests destination failure after source pop, read-only source guards,
80 concurrent identical-key opposed copies, distinct-target copies and
allocation rollback at each stage. Existing
page-replication fixtures additionally route middle, move and DB 7 copy events.
Ordinary List commands now use the page-aware DAO. Separate command, migration
and real replication fixtures complement this native storage coverage.


`paged_key_metadata` checks native List root metadata through the common DAO
boundary, independently of the List command tests. It covers TYPE,
EXISTS, key SCAN filtering, wrong types, MGET, TTL conditions and PERSIST with no
physical page value I/O. It checks metadata-only DB 7 observer groups, SET NX/XX
KEEPTTL, page reclamation on deletion/expiry/replacement, page-aware
COPY/RENAME, and preservation of invalid/future roots. The native PLR
signature is storage-wide; other payloads retain their configured codec.


`paged_list_dao` adds 600 operations against the legacy DAO, bounded native root
inspection, JSON-to-page migration of 1001 members including a 1 MiB member,
failed-command migration rollback, independent cross-DB COPY, RENAME/NX and
LMOVE/LMPOP precedence, and 400 concurrent copies while the source changes
representation. The server's existing List socket assertions now execute
the paged DAO. Live replication additionally checks real List command pages,
root-only expiry, v1 subscriber disconnection, cross-DB copy, rename and move.
`mdb` and `mdb_tbb` support pages; other backends reject List requests.
Hash/Set use Map pages, and ZSet uses coordinated member/score pages.


## Bounded Hash/Set index component

`bounded_map_store` tests the native AVL field/member index, bounded inline/blob
formats, 1600 `std::map` model steps, 10000 sorted fields, large binary payloads,
point-update locality, allocation rollback, complete page ownership audits and
4-thread/16-fiber read-own-writes. Damage fixtures check bad counts/heights,
missing pages, self links, fragment offsets, future versions and id exhaustion.
Both deletion orders reach an empty map without orphan pages. This storage
component is used by Hash/Set and the embedded ZSet indexes.
See `doc/MAP_STORAGE.md`.


`bounded_map_iteration` covers stable-bucket scan cursors under insertion and
deletion, early/key-only visitors, MATCH/NOVALUES value-page avoidance and
subtree skipping on 10000 fields. It tests bounded-buffer clone comparison of
same-bucket fields over 1 MiB, same/cross-DB independent ownership, scalar id
alias checks, allocation rollback, destination reset, DB 7 native page events
and replica application. Root/node format v2 records bucket order explicitly;
the unused v1 lexical prototype is rejected, not reinterpreted. Hash metadata,
migration and command adapters are active for Hash/Set/ZSet.


`paged_hash_dao` compares 600 commands with the legacy DAO and inspects bounded
native roots. It checks large legacy migration, integer-error rollback,
malformed/duplicate/future record preservation, allocation failures, HLEN/TTL
without member page I/O, HKEYS/HSCAN NOVALUES without value-fragment reads,
independent cross-DB COPY over another type, RENAME/PERSIST and expiry cleanup.
Existing client and scan tests now execute the native Hash DAO. Live replica
fixtures inspect actual Hash groups and verify large binary fields, TTL,
cross-DB copy, HINCRBY/HDEL, RENAME and PERSIST on a real receiver.

## oneTBB native storage

With `HAS_TBB=ON`, `tbb_native_storage` adds binary-key, raw overwrite, atomic
multi-root/page rollback, allocation-failure/retry and SCAN churn coverage.
Four threads each run four fibers exercising native Hash/List and yielding
multi-key transactions. The five Python server fixtures select `mdb_tbb` through
`PKV_TEST_BACKEND`; standalone fixtures still default to `mdb`.
The complete matrix has 29 CTests with oneTBB, 28 without it.
`live_page_replication` also exercises 100 shutdown cycles, including disconnect
before the writer starts and while its reader is blocked. The writer waits for
reader completion before releasing the socket event.

`PKV_BUILD_BENCHMARKS=ON` with oneTBB builds `memory_backend_bench`. It compares
legacy JSON/native Hash on both memory engines at 1/4 threads in one binary.
See [scope and results](../doc/TBB_STORAGE.md).

## Native Set commands

`paged_set_dao` compares 600 deterministic operations with the legacy DAO,
including repeated inputs and source/destination aliasing. It checks bounded
roots, metadata-only SCARD, subtree rank selection against traversal, small
sampling locality on 10000 members, pop/removal invariants and signed limits.
Fault sweeps verify SMOVE, aliased-input STORE and SPOP atomic rollback. Migration
fixtures include binary/1 MiB members, exact TTL, rejected commands, malformed
records and independent COPY/RENAME. Mixed Set/ZSet tests check aliased STORE
results above 16 KiB and reclamation of overwritten Set pages.

Existing socket, cluster and concurrent Set tests now execute `paged_set`.
Subscriber fixtures inspect PMR Set roots and bounded member fragments, TTL-only
root events and v1 disconnects. A real source/receiver pair verifies large/binary
Sets after SMOVE, UNIONSTORE, cross-DB COPY, RENAME and PERSIST. See
[Set storage scope](../doc/SET_STORAGE.md).

## Native ZSet storage component

`bounded_zset_store` tests the new PZR dual-index component. It checks more than
10000 numeric score encodings, 600 model operations per tested engine, complete
AVL/node/blob ownership audits and orphan detection. Rank/select, stable member
scan, binary and 1 MiB members, signed zero/infinities, point-update locality,
root-only expiry, allocation rollback, abandoned contexts, corrupt score values
and exhausted page IDs have direct coverage. Four threads each run four fibers,
yielding inside two-key transactions and checking both counters remain equal.
With oneTBB enabled, model/fault/concurrency/corruption fixtures also use that
engine. This adds the `bounded_zset_store` CTest to each build.

This is a storage component test. Native command/migration/replication coverage
is supplied by `paged_zset_dao` and the process-level fixtures.
See [native layout and remaining integration](../doc/ZSET_STORAGE.md).

## Native ZSet ranges and copying

`native_zset_ranges` compares 2400 range/count queries per engine against the
existing JSON view, covering equal/mixed scores, rank/score/lex bounds,
reverse/LIMIT/OFFSET, empty/binary/large members and exclusive endpoints.
Signed extremes and NaN bounds have explicit cases. Clone fixtures check 1 MiB
members, independent same/cross-DB ownership, exact TTL, arbitrary destination
replacement, staged source edits, missing/self copies, inconsistent-index
rejection and allocation rollback. A 10000-member fixture bounds page reads for
score counts, distant rank seeks, offset-heavy tiny results and uniform-score
lex counts. Mixed-score lex queries scan the member index with bounded top-k
response candidates; no general logarithmic lex-access claim is made.
This adds the `native_zset_ranges` CTest to each build. Enabled builds run both
memory engines. Native server integration is additionally covered by
`paged_zset_dao` and the process-level command/replication fixtures.

## Default native ZSet DAO

`paged_zset_dao` compares 600 commands with the legacy DAO per engine and checks
bounded roots after each step. It covers ZADD flags/increment, removals, pop,
ranges/counts, destination aliases and weighted algebra. Migration tests retain
exact TTL and signed zero with a 1 MiB member, reject NaN increments atomically,
and exercise generic COPY/RENAME/PERSIST plus full paginated ZSCAN. Expired-root
rebuilds must reclaim old member fragments. Invalid/future records and random
sampling have direct checks. Allocation sweeps cover ZADD, UNIONSTORE,
ZRANGESTORE and ZMPOP without partial roots/pages/results. Enabled builds execute
these fixtures against both `mdb` and oneTBB.

The existing client and cluster ZSet tests now use the native DAO, including
concurrent commands. Subscriber fixtures inspect PZR roots and bounded pages,
root-only TTL and v1 rejection. A real source/receiver pair verifies large
members, signed zero, score updates, UNIONSTORE, RANGESTORE, pop, cross-DB COPY,
RENAME and PERSIST. See [current native storage](../doc/ZSET_STORAGE.md).

## RocksDB native storage and recovery

`HAS_ROCKSDB=ON` adds `rocksdb_native_storage` and (on Unix with Python)
`rocksdb_crash_recovery`. The native fixture verifies legacy default-CF keys,
preserved other CFs, binary values, empty/page-shaped user keys, maximum page
IDs, range reclamation, index rebuild, persistent scans and all four structured
types after reopen. A raw column-family inspection checks actual root/page sizes.
Multi-path mutation preflight must leave both roots/pages unchanged, while
read-only inputs on another shard may participate in a single-shard write.

The crash fixture launches a separate writer, kills it after an acknowledged
epoch while later two-root/64-page writes continue, then checks complete recovered
roots/pages/indexes. Six cases give process-kill/WAL evidence, not power-loss
coverage. In RocksDB-only builds the real-server fixtures select
`rdb` with isolated temporary directories; oneTBB keeps precedence when both
features are enabled. The memory suite count remains 28, oneTBB 29, RocksDB-only
30. Local disk builds use C++17 Debug ASan/UBSan and Release with RocksDB 8.3.2
RTTI enabled. See [storage limits](../doc/ROCKSDB_STORAGE.md).


## WiredTiger native storage and recovery

`HAS_WT=ON` adds `wiredtiger_native_storage` and, on Unix with Python,
`wiredtiger_crash_recovery`. They cover legacy LSM table import, binary/empty
keys, bounded collection pages, maximum page IDs, root/page namespace isolation,
family truncation, bootstrap retry, SCAN rebuild, reopen and cross-path mutation
preflight. Six separate-process killed-writer cases check complete recovered
two-root/64-page epochs and scan entries. ASan checks cursor buffer lifetimes. The WiredTiger socket fixture allows
180 seconds for its many individually synced transactions; memory/RocksDB
timeouts are unchanged.

WiredTiger-only builds select `wdb` for the real-server fixtures, using
isolated temporary paths. Backend precedence with combined features is oneTBB,
RocksDB, WiredTiger, then built-in memory. The WiredTiger-only suite has 30 tests;
local builds use C++17 Debug ASan/UBSan and C++11 Release with WiredTiger 10.0.2.
See [storage and recovery limits](../doc/WIREDTIGER_STORAGE.md).


## Hash random-field sampling

`hash_sampling` runs HRANDFIELD assertions against pkv and the optional local
Redis reference. It covers single/array replies, distinct and repeated samples,
WITHVALUES, empty/binary fields, large values, TTL, error precedence, pipelines
and coherent snapshots during concurrent HSET commands. Two pkv-only impossible
response-size cases must fail without changing the Hash or closing the client.
Cluster routing also exercises both single-field and WITHVALUES forms.

`paged_hash_dao` adds fixed-seed rank sampling, native mdb/oneTBB semantic checks,
legacy migration allocation-failure sweeps and 1024-field page-locality checks.
Three-field samples read fewer than 150 pages and field-only samples load no
value fragments. Existing Set and ZSet tests exercise their shared rank sampler.
The new process fixture adds one CTest per build: memory 29, oneTBB 30 and
single-disk-backend 31. See [sampling design](../doc/HASH_SAMPLING.md).


## Floating-point increments

`float_increment_storage` checks long-double parsing/formatting, maximum finite
values, nonzero subnormals, locale restoration, finite overflow, native Hash
point-update locality, oversized-value rejection without fragment reads, key
TTL, failed String I/O and Hash migration/native allocation rollback. Enabled
oneTBB builds exercise both memory engines.

`float_increment_commands` checks INCRBYFLOAT and HINCRBYFLOAT on pkv and the
optional local Redis reference, plus byte-exact reply/stored-value comparisons
for edge cases and 200 generated pairs. Concurrent increments must return each
expected result exactly once. Process fixtures use the selected native backend
with isolated temporary paths. `page_live_stream` verifies final computed
String/Hash payloads; cluster tests check routing before numeric errors.

The two new fixtures raise suite counts to memory 31, oneTBB 32 and one enabled
disk backend 33. See [design and validation limits](../doc/FLOAT_INCREMENTS.md).

The final eight-build matrix passed 258 entries. `float_increment_commands`
passed 889 common checks per server and 556 exact reply/stored-value comparisons.
The initial RocksDB Debug run timed out its socket fixture during concurrent
WiredTiger writes; a complete isolated rerun passed all 33 under the unchanged
45-second socket timeout. Final source-width, Python syntax and diff-whitespace
checks also passed.


## Native String component (historical rollout stage)

`native_string_pages` checks the PSR codec, a 500-step std::string model on mdb
and optionally oneTBB, binary/empty roots, page boundaries, localized edits,
zero-filled growth, exact TTL, cross-DB clone independence, malformed/future
records, ignored errors, forgotten finish and allocation-failure rollback.
An 8-MiB fixture limits individual allocations to 32 KiB during small range and
point-write operations and verifies exact touched-page counts.

Disk-enabled builds add `rdb_string_pages` or `wdb_string_pages`, using a Python
managed temporary directory and three open/close lifetimes to verify retained
bytes, cross-page edits, empty Strings and TTL. No server String command changes
are implied by these component tests. See [rollout scope](../doc/STRING_PAGES.md).

All eight builds compiled the String component; 28 targeted entries passed
(native String plus existing page/record regressions, with optional disk reopen
fixtures). The expanded manifests now contain 32 memory tests, 33 with oneTBB
and 35 with one disk backend. These full matrices were not rerun in this stage;
the default String command path is still unchanged.


## Default native String DAO

`paged_string_dao` compares 500 operations per memory engine with the legacy
DAO and checks bounded PSR roots, generic metadata, exact TTL, empty Strings,
COPY/RENAME/PERSIST, conditional SET, MGET ordering/nils and MSETNX preflight.
Allocation sweeps cover SET GET, legacy migration and atomic MSET over two
owners. The DAO-level 8-MiB fixture constrains allocations and verifies only
three chunk reads/two writes for small ranges, point edits and append;
STRLEN/TTL touch no data pages.

The server now selects this DAO. Existing command/cluster fixtures exercise it
on the configured native backend. Real replication fixtures inspect PSR roots,
large String edits, cross-DB clone/rename and TTL. Old peers retain bounded
inline String events; large Strings require PKV2. Queue-limit tests use PKV2
for large payloads and separately test legacy JSON expansion plus unsupported
metadata-only native records. The full per-build counts are now 33 without
optional engines, 34 with oneTBB and 36 with one disk backend.


The final native-String integration matrix passed 278 entries across all eight
builds (33/33/34/34/36/36/36/36). The initial highly concurrent memory sanitizer
run had a startup timeout; the complete reduced-concurrency rerun passed all 33
without changing its deadline or assertions. Both final WiredTiger matrices ran
sequentially and passed all 36. See [validation details](../doc/STRING_PAGES.md)
and the [DAO benchmark](../doc/STRING_STORAGE_BENCHMARK.md) for scope and limits.


## Native bitmap commands

`bitmap_storage` checks the default paged String DAO against a 500-update byte
model per enabled memory engine, shared-byte thread races, maximum offset
validation, exact TTL, native/legacy allocation rollback, failed commits and
corrupt/future records. Its 8-MiB locality fixture caps individual allocations
at 32 KiB and asserts exact page reads/writes for point/range and full scans.

`bitmap_commands` uses the selected native backend. It checks GETBIT/SETBIT,
BYTE/BIT ranges for BITCOUNT/BITPOS, empty/missing differences, signed bounds,
error precedence, zero growth, expiry, pipelining, generated range models and
shared-byte socket races. The optional local Redis reference runs the common
checks and exact range comparisons. Existing cluster and real PKV2 replication
fixtures also cover bitmap commands. The reference reports modified=1, so this
is not independent stock Redis certification.

Disk-enabled `rdb_bitmap_reopen`/`wdb_bitmap_reopen` use three connection lifetimes
in isolated paths to check retained bit updates, count, position and key TTL.
Suite manifests now contain 35 memory tests, 36 with oneTBB and 39 with one
disk backend. See [bitmap design and validation](../doc/BITMAPS.md).


The final bitmap full matrix passed all 298 CTest entries across eight builds
(35/35/36/36/39/39/39/39), with no failures or retries. Every configured server
and the local Redis reference passed 1,632 common bitmap checks; each build
also passed 1,812 exact range comparisons. Source width/function size, Python
syntax and CRLF-aware diff whitespace checks passed. See the bitmap validation
record for build scope and limitations.


## Native BITOP

`bitop_storage` checks all eight operators with an independent per-bit count
model over 200 operations per enabled memory engine. It covers duplicate input
multiplicity, aliases, read-only legacy roots, TTL, destination type replacement,
wrong-type preflight, missing/future input protection and allocation rollback.
It also checks the complete staged-family seal, ignored producer failures,
forgotten finish, stale tail reclamation and threaded alias updates. An 8-MiB
output test caps individual allocations at 64 KiB and asserts exact input and
output page counts; this still permits output-sized transaction staging.

`bitop_commands` covers operator/arity errors, zero padding without trimming,
empty deletion, binary keys, inline/page boundaries, aliases, pipelines and
concurrent coherent source snapshots while MSET updates both keys. It runs the
common cases and generated exact result/GET/TTL comparisons against the optional
local Redis reference. Cluster and actual PKV2 replication tests also exercise
BITOP. The reference reports modified=1; this is not stock-release certification.

`rdb_bitop_reopen`/`wdb_bitop_reopen` check alias output through three connection
lifetimes, plus read-only legacy/native sources on two physical paths and a
single-path output. Suite manifests contain 37 memory tests, 38 with oneTBB
and 42 with one disk backend. See [BITOP design](../doc/BITOP.md).


The BITOP full matrix passed 318 CTest entries across eight configurations
(37/37/38/38/42/42/42/42). Each server/reference passed 1,198 common BITOP checks
and each build passed 384 exact generated result/GET/TTL comparisons. Nine
supplemental entries passed after strengthening rollback and replica-deletion
assertions; subsequent WiredTiger matrices already included those changes.
See [BITOP validation](../doc/BITOP.md) for scope and the precheck correction.


## Native BITFIELD

bitfield_storage checks field arithmetic, sequential suboperations, read-only
legacy records, native/legacy allocation rollback, page locality, shared-field
increments and synthetic maximum-start tails. Disk-enabled bitfield_reopen
fixtures retain normal fields, TTL and that tail neighborhood over three
connection lifetimes; the synthetic tail intentionally omits unrelated pages.

bitfield_commands uses arbitrary-precision models and exact local Redis reply/
stored-byte comparisons, signed/unsigned extrema, overflow policies, strict
errors, pipelines, checked indexed offsets and socket races. Cluster and real
PKV2 replication tests cover field updates and all-FAIL zero growth. New source
limits remain 80 columns/200 lines per function. Manifests now contain 39 memory
tests, 40 with oneTBB and 45 with one disk backend. See
[BITFIELD scope and validation](../doc/BITFIELD.md).


The final BITFIELD full matrix passed 338 CTest entries across eight builds
(39/39/40/40/45/45/45/45), without failed fixtures or retries. Each configured
server/reference passed 1,276 common checks, each build matched 200 generated
reply/final-byte comparisons, and four pkv-only checked-index cases passed.
The initial directed run found the Redis signed SET SAT priority corner; see
[validation details](../doc/BITFIELD.md) for the correction, storage-cap extension,
reopen fixture scope and remaining compatibility limits.

## Client transactions and WATCH

`transaction_commands` checks MULTI/EXEC/DISCARD, binary argument ownership across
parser reuse, pipelined/nested replies, runtime errors with continued execution,
queue-time EXECABORT, SELECT ordering and database persistence, optimistic
conflicts, repeated WATCH, UNWATCH, expiry, missing keys and disconnect cleanup.
Both pkv and the local modified=1 Redis 8.6.2 reference run 134 shared assertions,
105 successful optimistic increments with retries, and concurrent two-key EXEC
writes observed by coherent MGET reads. Twenty-nine additional pkv-only cluster
checks verify per-command routing and aggregate EXEC preflight before writes.
Four independent checks reject NUL-suffixed transaction controls. The ordinary
socket fixture has twelve independent strict command-name checks; the old shared
GET-with-NUL acceptance check was removed because pkv now intentionally rejects
all NUL-containing names, while the local reference accepts some such names.

`execution_gate` exercises reentrancy and exception unwinding, parallel shared
admission, four OS threads with four fibers each, DB calls nested under EXEC,
scheduler reuse and the frozen EXEC clock. `transaction_queue` verifies owned
binary snapshots, command/byte limits, allocation failure without partial queue
capture, response ownership transfer under allocation failures, and key layouts
that distinguish real keys from COUNT, weights, destinations and other operands.
`watch` checks all five legacy migrations without optimistic invalidation,
same-value writes, stale subscription cleanup, missing epochs, malformed
metadata, failed-commit preservation and replica application. The page-stream
fixture checks ROOT_MAINTENANCE capture/decoding and rejects a maintenance marker
without a page-family reset.

These checks establish in-process transaction isolation. Source whole-EXEC
durability remains open; PKV3 envelope tests are described below. See
[transaction behavior and limits](../doc/TRANSACTIONS.md).

Before ordered-sequence work, the transaction matrix passed 370/370 CTests
across eight builds.

| Build | Passed |
| --- | ---: |
| memory | 43 |
| c11 | 43 |
| tbb | 44 |
| tbb-c11 | 44 |
| rocks | 49 |
| rocks-release | 49 |
| wt | 49 |
| wt-c11 | 49 |

Memory, oneTBB, RocksDB and WiredTiger Debug builds use ASan/UBSan. The
external RocksDB/WiredTiger libraries themselves are not sanitizer builds.
The C++11 memory/oneTBB/WiredTiger and C++17 RocksDB Release builds also pass.
Final logs are `/tmp/pkv-txn-<build>-full.log`. The strict-name transition
initially failed the old shared smoke expectation; the final matrix uses the
explicit independent safety checks described above.

## Ordered page sequences

`page_sequence` verifies atomic replay of ordered batches in one database:
root replacement, deletion/recreation, invalid intermediate prefixes, maintenance
and missing-key WATCH history. A model applies 100 groups of 20 updates against
an independent root/page map. Allocation sweeps verify unchanged state and WATCH,
zero old-page reads while staging, and at most one engine commit. Append failures
and byte/record limits are checked separately. Single-batch replay retains its
direct path without allocating the multi-batch state map.

`page_sequence_tbb`, `page_sequence_rdb` and `page_sequence_wdb` run the same
physical replay cases for each enabled native backend. The disk wrappers also
close the writer process, reopen the final checkpoint in another process, and
check that a sequence spanning independent physical stores is rejected without
changing data or WATCH. These storage tests run independently of PKV_SOCKET_TESTS
when Python is available. They do not establish whole-EXEC network envelopes,
cross-database atomic commits or source-side whole-EXEC durability. See
[the storage primitive and remaining work](../doc/PAGE_SEQUENCES.md).

The ordered-replay full matrix passed 384/384 CTests before the final
missing-root maintenance WATCH correction. That correction, including a
migration-created key deleted in the same group, was then rebuilt and tested
with 46/46 affected CTests across the same eight configurations. These include
sequence, WATCH, page transport/live replication and transaction protocol paths.
The final test-only line wrap changes no behavior.

| Build | Full matrix | Final affected-path checks |
| --- | ---: | ---: |
| memory | 44 | 5 |
| c11 | 44 | 5 |
| tbb | 46 | 6 |
| tbb-c11 | 46 | 6 |
| rocks | 51 | 6 |
| rocks-release | 51 | 6 |
| wt | 51 | 6 |
| wt-c11 | 51 | 6 |

Logs: `/tmp/pkv-sequence-<build>-full.log` and
`/tmp/pkv-sequence-maint-<build>-target.log`. The first native oneTBB sequence
run exposed rejection of page cleanup for a missing owner; preparation now
handles those erases as no-ops. Explicit pre-fix checks also reproduced both
missing-root migration WATCH creation and creation/delete history failures.
The final affected-path checks include those regression cases for each backend.
External RocksDB/WiredTiger libraries remain unsanitized dependencies even in
the ASan/UBSan server builds. Whole-EXEC recovery work remains open.

PKV3 adds `exec_replication` and `exec_live_stream`; see
[EXEC replication](../doc/EXEC_REPLICATION.md) for coverage and remaining limits.
The historical ordered-replay counts above predate these two tests per build.

The final PKV3 matrix passed **400/400 CTests** across eight builds:
memory/C++11 memory (46 each), oneTBB/C++11 oneTBB (48 each), and
Debug/Release RocksDB plus Debug/C++11 WiredTiger (53 each). Debug memory,
oneTBB, RocksDB and WiredTiger server builds use ASan/UBSan; external engine
libraries remain unsanitized. Logs are `/tmp/pkv-exec-<build>-full.log`.
A final test-only line wrap was followed by a rebuilt memory EXEC unit run;
the other seven full runs include that same final source. Native allocation
faults cover raw/page observers, final group construction, allocation-free
subscriber close and nested exception unwinding. The first non-escalated
loopback attempt failed on sandbox socket permissions; all full runs above
used permitted local sockets. No test timeout was treated as successful exit.

`page_workspace` adds source-side staging, read-through/reset/model checks,
all-five-DAO large values, legacy migration/WATCH, limits, discard, observer
failure and allocation sweeps. `page_workspace_tbb`, `_rdb` and `_wdb` run the
native cases with independent disk reopening where applicable. See
[workspace semantics and integration limits](../doc/PAGE_WORKSPACES.md).

The workspace matrix passed **414/414 CTests** before the final two
formatting-only line wraps. All eight configurations were then rebuilt and
passed **68/68 affected-path CTests** for workspace/replay, WATCH, native page
transport and actual EXEC replication. The final memory workspace fixture also
includes the later test-only capability/rejection additions; it was rebuilt and
rerun before the formatting pass. The other seven full runs contain those
additions. No runtime source changes followed the two line wraps.

| Build | Full suite | Final affected paths |
| --- | ---: | ---: |
| memory | 47 | 7 |
| c11 | 47 | 7 |
| tbb | 50 | 9 |
| tbb-c11 | 50 | 9 |
| rocks | 55 | 9 |
| rocks-release | 55 | 9 |
| wt | 55 | 9 |
| wt-c11 | 55 | 9 |

Full logs: `/tmp/pkv-workspace-<build>-full.log`. Final rebuilt checks:
`/tmp/pkv-workspace-style-<build>-target.log`. Debug memory, oneTBB, RocksDB
and WiredTiger builds use ASan/UBSan; external disk-engine libraries remain
unsanitized. C++11 configurations cover memory, oneTBB and WiredTiger. All build
and test sessions reached successful terminal status. These checks do not prove
live source EXEC integration, cross-DB coordination or power-loss durability.

`source_exec` now drives actual handler staging, one source commit, reply
failure policy, SELECT/COPY restoration, runtime errors, conservative fallback
and allocation sweeps. `source_exec_rdb`/`_wdb` terminate at engine commit
boundaries and after acknowledgement, then reopen all five stored data types in
a different process. The real TCP EXEC fixture adds native metadata transfers,
expiry, scans, source WATCH history and post-EXEC behavior. See
[source commit guarantees and limits](../doc/SOURCE_EXEC.md).

The first final oneTBB C++11 full run hit a healthy-subscriber close in
`replica_queue_limits` during concurrent builds; the unchanged test passed in
isolation. Its producer had no limit on lag behind the Python healthy reader,
so the test could legitimately overflow that reader's 2-MiB queue. The fixture
now waits for healthy-reader progress every 64 records (each about 8 KiB),
keeping even an in-flight plus queued window below quota. The second subscriber
still reads none of the 1500-record/12-MiB stream. Memory/oneTBB paced checks and
the full oneTBB C++11 retry pass. The initial failure log is preserved at
`/tmp/pkv-exec-final-tbb-c11-initial-full.log`; isolated and paced logs are
`/tmp/pkv-exec-final-<build>-queue-*.log`.


The optimized source EXEC implementation passed **426/426 CTests**. A later
legacy-expiry boundary regression showed that an old whole-value record just
below 16 KiB can grow past the workspace limit when its expiry is rewritten.
The final eligibility correction preserves the compatible direct path for
whole-value legacy key writers. All eight server/test configurations were
rebuilt and passed **28/28 affected-path tests** after that correction; the full
426-test suite was not repeated after this final change.

| Build | Full source EXEC matrix | Final affected paths |
| --- | ---: | ---: |
| memory | 48 | 3 |
| c11 | 48 | 3 |
| tbb | 51 | 3 |
| tbb-c11 | 51 | 3 |
| rocks | 57 | 4 |
| rocks-release | 57 | 4 |
| wt | 57 | 4 |
| wt-c11 | 57 | 4 |

Full logs are `/tmp/pkv-exec-final-<build>-full.log`; final rebuilt checks are
`/tmp/pkv-exec-legacy-<build>-target.log`. The failing legacy boundary
reproduction is preserved in `/tmp/pkv-exec-legacy-before.log`. Every final
build/test process reached successful terminal status. The final checks include
actual handler transactions, real TCP EXEC replication and disk process-crash
reopening where applicable. Debug builds use ASan/UBSan, with external storage
libraries unsanitized. See the local
[source EXEC measurements](../doc/SOURCE_EXEC_BENCHMARK.md) for performance
costs and measurement limits.


`legacy_expiry` checks native migration of old String/List/Hash/Set/Sorted Set
values when expiry or persistence changes. Coverage includes 70-KiB members,
binary/empty members, unchanged conditions, source WATCH, one observed bounded
batch, storage rejection/retry, nonpage compatibility and **456** C++ allocation
positions (58 String, 84 Hash, 79 List, 89 Set and 146 Sorted Set). Backend
variants add oneTBB and independent RocksDB/WiredTiger reopening. `source_exec`
now asserts one source commit for large old-format expiry plus another write,
and no source/WATCH changes when that final commit is rejected.

The first full runs found two tests assuming expiry retained the old encoding.
The float fixture now follows migration with the production native String DAO;
its legacy dbset-failure fixture explicitly disables native pages so the failure
still exercises the intended write API. Codec tests retain the custom-codec
length/range checks before migration and additionally check native reads after
migration. The initial allocation fixture also counted attempted commits as
successful; its counter now increments only after an engine success. Initial
failure logs are retained under `/tmp/pkv-expiry-*-full.log`,
`/tmp/pkv-expiry-*-final-full.log` and `/tmp/pkv-expiry-memory-oom.log`.
See [legacy migration semantics](../doc/LEGACY_EXPIRY.md).


The legacy-expiry implementation passed **440/440 CTests** across all eight
configurations. The only subsequent production change was a line wrap in the
transaction planner. All eight server/source_exec targets were rebuilt after
that formatting change and passed **8/8 source_exec checks**.

| Build | Full suite | Final formatting check |
| --- | ---: | ---: |
| memory | 49 | 1 |
| c11 | 49 | 1 |
| tbb | 53 | 1 |
| tbb-c11 | 53 | 1 |
| rocks | 59 | 1 |
| rocks-release | 59 | 1 |
| wt | 59 | 1 |
| wt-c11 | 59 | 1 |

Successful full logs are `/tmp/pkv-expiry-<build>-verified-full.log`, except
`tbb` and `rocks`, whose logs are `/tmp/pkv-expiry-<build>-final-full.log`.
Final formatting checks are `/tmp/pkv-expiry-style-<build>-target.log`.
All build/test handles reached terminal success. Debug memory, oneTBB, RocksDB
and WiredTiger use ASan/UBSan; external engine libraries remain unsanitized.
Memory, oneTBB and WiredTiger also have C++11 Release coverage. The matrix
includes real TCP commands/replication, source EXEC storage failure/crash tests,
legacy migration, allocation failures and independent disk reopening. It does
not establish Redis completeness, power-loss safety or a general performance
advantage over upstream Redis.


`legacy_transfer` adds bounded destination imports for enumerable old values
across COPY/RENAME/RENAMENX. It checks all five types with 70-KiB payloads,
exact TTL, binary/empty members, condition/same/missing cases, source/destination
WATCH, old orphan-page removal, rejected/unsupported commits, independent
cross-DB COPY, legacy engines/observers and read-only custom codecs. A custom
visitor-only view explicitly opts into native import without full collection
materialization. Captured COPY/RENAME batches replay into a second database;
disk variants reopen both destinations and check old orphan-page absence.

The memory fixture covers **962 allocation-budget positions** across ten DAO
cases (five types, COPY and RENAME). Actual-handler `source_exec` adds **237**
positions for legacy COPY/RENAME inside one staged EXEC, plus its existing SET
sweep. These counts include successful control budgets. Successful commits keep
all intended effects; earlier failures retain the old source, absent targets
and valid WATCH. The final handler commit is also explicitly rejected after
legacy transfer staging to verify no source prefix escapes.

The first targeted run found a copied Hash verifier still reading its old
source name, and an old cross-codec assertion requiring a text encoding after
native migration. The full memory/oneTBB runs then found a representation-race
reader using the legacy-only JSON String DAO for its now-native destination.
The target reader is now native; the writer still flips legacy String, legacy
List and native List. Initial logs remain at
`/tmp/pkv-transfer-memory-initial.log` and
`/tmp/pkv-transfer-<build>-full.log`. Compile diagnostics for an early missing
include and a temporary-to-reference fixture call were resolved before final
verification. See [transfer contract](../doc/LEGACY_TRANSFERS.md).


Before the final retry corrections, the transfer implementation passed
**454/454 full-matrix CTests**. A subsequent deterministic source lookup fixture
reproduced a second format change: enumerable Hash to old empty Hash to
enumerable Hash. The retry had remembered the opaque choice and could write a
large JSON destination. The final code rechecks the current capability on every
attempt. Another scripted lookup then removed the source at either read boundary,
exposing stale error status in RENAME/RENAMENX. Both callbacks now explicitly
report no_key on missing source. That regression covers native and legacy input
with NX enabled/disabled. All eight configurations were rebuilt and passed
**88/88 affected-path tests** after both corrections, including the new cases.
The full 454-test matrix was not repeated after these final corrections.

| Build | Full matrix before final corrections | Final affected paths |
| --- | ---: | ---: |
| memory | 50 | 9 |
| c11 | 50 | 9 |
| tbb | 55 | 11 |
| tbb-c11 | 55 | 11 |
| rocks | 61 | 12 |
| rocks-release | 61 | 12 |
| wt | 61 | 12 |
| wt-c11 | 61 | 12 |

Successful full logs are `/tmp/pkv-transfer-<build>-verified-full.log`, except
WT Debug, whose successful standalone retry is
`/tmp/pkv-transfer-wt-isolated-full.log`. Its concurrent full run reached the
180-second socket_compatibility timeout while the other 60 tests passed.
No matching pkv/driver process remained; the unchanged binary passed that case
in 97.51 seconds in its isolated full retry. No timeout, sync policy or test
expectation was changed for this retry. The concurrent failure remains at
`/tmp/pkv-transfer-wt-verified-full.log`.

Regression reproductions are `/tmp/pkv-transfer-format-before.log` and
`/tmp/pkv-transfer-missing-before.log`. The format-only correction first passed
`/tmp/pkv-transfer-final-<build>-target.log`; final rebuilt tests after both
corrections are `/tmp/pkv-transfer-rename-final-<build>-target.log`. All final
build/test handles reached successful terminal status. Debug memory, oneTBB,
RocksDB and WiredTiger use ASan/UBSan; external engine libraries remain
unsanitized. C++11 Release covers memory, oneTBB and WiredTiger. These checks
still do not establish cross-database atomicity, durable replication replay,
power-loss guarantees or a general performance advantage over Redis.

## Command metadata and RESP ownership

`command_catalog` verifies registry coverage and arities, constant metadata RESP
round trips, binary/large unknown names and concurrent readers. With
`PKV_REFERENCE_REDIS` configured, `command_metadata` compares INFO descriptors
and GETKEYS/GETKEYSANDFLAGS with an isolated reference, including counted keys,
SET/BITFIELD flags, malformed arguments, binary names and duplicates. It also
checks LIST filters, basic DOCS, no target execution, queued queries and cluster
routing. The test selects the enabled memory/TBB/RocksDB/WiredTiger backend.
See [command discovery limits](../doc/COMMAND_METADATA.md).

Four `resp_alloc_*` fixtures cover C++11/C++17 and array/vector caches. Each
sweeps allocation budgets through nested parsing, response construction and
ownership transfer with cache capacities 0, 1, 8 and 64. Explicit live allocation
counts check that both failures and successful controls release every owner.
The Debug memory run covers 1,852 budget positions per array-cache fixture and
1,849 per vector-cache fixture. These include successful control budgets.
`source_exec` adds 207 metadata allocation positions around writes and SELECT
changes, checking all-or-none source publication and one engine commit.

The new EXEC sweep initially exposed a real parser use-after-free when acquiring
the next object failed after the completed object had already been published.
The production parser now acquires first and publishes second, with cleanup on
publication failure. Root/child construction cleans up failed insertions, and
the vector cache reserves its bounded capacity so return/destruction does not
allocate. The initial ASan failure remains at
`/tmp/pkv-command-memory-source.log`; the corrected six-test targeted run is
`/tmp/pkv-command-memory-ownership.log`.

The final implementation passed **502/502 full-matrix CTests**, including the
previous legacy transfer retry corrections and the new command/ownership cases.

| Build | Passed |
| --- | ---: |
| memory | 56 |
| c11 | 56 |
| tbb | 61 |
| tbb-c11 | 61 |
| rocks | 67 |
| rocks-release | 67 |
| wt | 67 |
| wt-c11 | 67 |

Logs are `/tmp/pkv-command-<build>-full.log`. All eight test sessions reached
terminal success. The WiredTiger suites ran separately after the other six
finished; their socket compatibility cases passed with the existing timeout
and synchronous logging. No timeout or durability setting was changed. Debug
memory/TBB/RocksDB/WiredTiger use ASan/UBSan; external libraries remain
unsanitized. Every configuration completed 300 key-discovery comparisons
against the local modified=1 Redis reference. These checks do not establish
full Redis compatibility or a throughput advantage over upstream Redis.

## LCS

The `lcs` fixture checks 800 generated binary pairs against an independent
small dynamic-programming model in default/LEN/IDX modes. It reconstructs the
subsequence from IDX ranges, checks dimension overflow/admission, and enforces
allocation ceilings that exclude a full uint32 matrix. A 70-KiB old/native
input verifies read-only legacy handling, unchanged WATCH and all three result
allocation paths. The memory fixture sweeps 89 allocation positions including
successful controls; oneTBB builds repeat those cases on both memory engines.
Injected native page errors leave inputs readable, and over-limit requests
read no payload pages. Concurrent MSET toggles test coherent two-key reads.

With `PKV_REFERENCE_REDIS` configured, `lcs_commands` compares the complete
option set, match ranges/tie order, binary data, expiry, wrong-type/option error
precedence, logical DBs and staged EXEC with an isolated local Redis reference.
The test selects the enabled TBB/RocksDB/WiredTiger backend, or default memory.
NUL-suffixed options are rejected in a separate PKV-only assertion, since the
local modified=1 reference accepts truncated options. Same-slot execution and
ordinary/queued cross-slot rejection run against PKV cluster mode.

The initial ordinary cross-slot test found that the String route classified
only the first LCS key. That implementation failure is retained in
`/tmp/pkv-lcs-memory-target.log`; the corrected affected paths passed seven
CTests in `/tmp/pkv-lcs-memory-route-target.log`. Expanded allocation/read-fault
coverage passed in `/tmp/pkv-lcs-memory-expanded-target.log`. New source files
require CMake reconfiguration before building; an initial build without it
failed to link the newly introduced virtual method and was reconfigured.
See [LCS design and limits](../doc/LCS.md).

The final implementation passed **518/518 full-matrix CTests**:

| Build | Passed |
| --- | ---: |
| memory | 58 |
| c11 | 58 |
| tbb | 63 |
| tbb-c11 | 63 |
| rocks | 69 |
| rocks-release | 69 |
| wt | 69 |
| wt-c11 | 69 |

Logs are `/tmp/pkv-lcs-<build>-full.log`; all test sessions reached terminal
success. Each configuration completed 1,112 LCS differential assertions, plus
the separate PKV-only binary-option and cluster checks. Debug memory, oneTBB,
RocksDB and WiredTiger use ASan/UBSan; external libraries remain unsanitized.
Memory, oneTBB and WiredTiger also have C++11 Release coverage. WiredTiger
full suites ran independently after the other six finished, with unchanged
timeouts and synchronous logging. These results validate the tested LCS scope;
they do not establish complete Redis compatibility or general throughput
superiority over Redis.

## KEYS and native metadata inspection

`keyspace` checks exhaustive matching, workspace deduplication, unchanged live
WATCH and legacy bytes, concurrent atomic key transfers, native payload-read
avoidance, rejected scans and nonadvancing cursors. Its C++ allocation sweep
covers 1,249 positions including the successful control. The fixture also
checks heap-free metadata inspection for all five native types, contrasts it
with the owning-view allocation requirement, and compares both validation paths
under byte mutations. oneTBB builds repeat traversal/concurrency on both memory
engines. `keyspace_commands` selects the enabled backend and compares 336
assertions with an isolated local Redis reference, covering binary glob syntax,
all supported types, logical DBs, TTL, WATCH and staged EXEC. Separate cluster
checks verify node-local behavior, including an unserved initial keyspace.

The first protocol run incorrectly treated a queued KEYS reply as a key array;
the fixture now normalizes only array replies. That failed run is retained at
`/tmp/pkv-keys-memory-target.log`. The corrected shared metadata path passed
nine affected tests in `/tmp/pkv-keys-memory-final-target.log`. See the
[keyspace contract](../doc/KEYSPACE.md) for blocking/memory costs and scope.

The feature passed **534/534 full-matrix CTests**, plus **8/8 final
formatting-path fixtures**:

| Build | Full suite | Formatting-path fixture |
| --- | ---: | ---: |
| memory | 60 | 1 |
| c11 | 60 | 1 |
| tbb | 65 | 1 |
| tbb-c11 | 65 | 1 |
| rocks | 71 | 1 |
| rocks-release | 71 | 1 |
| wt | 71 | 1 |
| wt-c11 | 71 | 1 |

Full logs are `/tmp/pkv-keys-<build>-full.log`; detailed outputs are preserved
at `/tmp/pkv-keys-<build>-full-details.log`. Every configuration completed 336
KEYS differential assertions, plus the separate cluster/EXEC checks. The six
non-WiredTiger full suites ran before a final whitespace-only line split in
`json_scan.cpp`. All eight were rebuilt and passed the keyspace fixture in
`/tmp/pkv-keys-<build>-style-target.log`; both WiredTiger full suites then ran
with that final formatting. No semantic change followed the full suite runs.
All final build/test sessions reached terminal success.

WiredTiger full suites ran independently with unchanged timeouts and synchronous
logging. Debug memory/TBB/RocksDB/WiredTiger use ASan/UBSan; external libraries
remain unsanitized. Memory, TBB and WiredTiger also have C++11 Release coverage.
These checks do not establish complete Redis compatibility, active expiry or
a general throughput advantage over Redis.

## DBSIZE and stored root counts

`key_count` compares counters with raw root enumeration across overwrite,
missing delete, duplicate legacy replication, large native pages, TTL changes,
expired-root cleanup, workspace histories/discard and rejected-commit retry.
Its memory allocation sweep covers 89 budgets including a successful control;
oneTBB builds repeat it on both memory engines. Concurrent writers create and
overwrite distinct keys while queries check coherent totals, then cleanup
checks the final zero. A spy rejects root reads/scans during DBSIZE and checks
its mdb path with C++ allocation disabled.

Backend variants add native engine execution, smaller concurrent disk workloads,
independent-process reopen and aggregation across two physical stores. An
unsupported cross-store write must leave both roots and count unchanged.
`dbsize_commands` exercises all five types with 70-KiB payloads, binary/empty
keys, COPY/RENAME, logical DBs, WATCH, source EXEC and DISCARD. It runs the same
expected responses against the local Redis reference and the selected PKV
backend; cluster cases verify node-local counts without served slots. See the
[counting contract and startup cost](../doc/DBSIZE.md).

The final implementation passed **556/556 full-matrix CTests**:

| Build | Passed |
| --- | ---: |
| memory | 62 |
| c11 | 62 |
| tbb | 68 |
| tbb-c11 | 68 |
| rocks | 74 |
| rocks-release | 74 |
| wt | 74 |
| wt-c11 | 74 |

Full logs are `/tmp/pkv-count-<build>-full.log`; detailed outputs are preserved
at `/tmp/pkv-count-<build>-full-details.log`. All final build/test sessions
reached terminal success. Every configuration ran 40 expected DBSIZE replies
per PKV/reference server, plus the separate error/discovery/cluster assertions.
Native backend fixtures passed independent reopening and cross-store count
checks where supported. Both WiredTiger full suites ran independently after
the other six finished, with unchanged timeout and synchronous logging.

Debug memory/TBB/RocksDB/WiredTiger use ASan/UBSan; external libraries remain
unsanitized. Memory, TBB and WiredTiger also have C++11 Release coverage. These
checks do not establish full Redis compatibility, active expiry, power-loss
guarantees or a general throughput advantage over upstream Redis.

## Active expiry

`active_expiry` checks a bounded, striped TTL index; steady expiration without
full rescans; all five legacy/native types with 70-KiB payloads; native page
reclamation; mutation/replication/workspace index updates; stale deadline
rechecks; corrupt/persistent/future root preservation; rejected-delete retry;
absent WATCH; and EXEC isolation. It also checks worker stop/join and that
inventory of opened logical DBs never creates unopened ones. Memory allocation
sweeps cover eight post-commit and twelve snapshot-rebuild budget positions,
including successful controls. oneTBB repeats native/legacy and publication
cases on both memory engines. `keyspace` additionally verifies heap-free inline
String metadata inspection and its validation parity with owning views.

Backend variants independently reopen disk data, check root/page removal and
exercise expiry across independent physical stores. `active_expiry_cleanup`
observes physical deletion via DBSIZE without reading expired keys, checks
renewal/PERSIST and staged SET, explicit disable and passive receiver behavior,
and verifies a real primary expiry deletion reaches its receiver. It selects
the enabled backend. Existing server suites run with active expiry enabled by
default. See [configuration, repair and limits](../doc/ACTIVE_EXPIRY.md).

The first targeted six-test run passed at
`/tmp/pkv-active-memory-target.log`; expanded failure/rebuild/native-replication
coverage also passed six tests at
`/tmp/pkv-active-memory-expanded-target.log`. No runtime failure was reproduced
in these targeted runs. Early style checks found overlong new lines, which were
split before the final build matrix. Existing CRLF master-service sources were
edited as bytes and retain their line endings.

The final implementation passed **578/578 full-matrix CTests**:

| Build | Passed |
| --- | ---: |
| memory | 64 |
| c11 | 64 |
| tbb | 71 |
| tbb-c11 | 71 |
| rocks | 77 |
| rocks-release | 77 |
| wt | 77 |
| wt-c11 | 77 |

Full logs are `/tmp/pkv-active-<build>-full.log`; detailed outputs are preserved
at `/tmp/pkv-active-<build>-full-details.log`. All final build/test sessions
reached terminal success. Every configuration passed real TCP physical cleanup,
renewal, disable/passive-role and primary-to-receiver expiry deletion checks.
Disk variants passed independent reopening and cross-store fallback checks.
Both WiredTiger full suites ran independently after the other six finished;
no timeout, expectation or durability policy was weakened.

Debug memory/TBB/RocksDB/WiredTiger use ASan/UBSan; external libraries remain
unsanitized. Memory, TBB and WiredTiger also have C++11 Release coverage. The
service constructor's interval validation was aligned with the startup config
bounds before this full matrix, and all eight rebuilt expiry fixtures passed.
These results do not establish complete Redis compatibility, durable replication
replay, power-loss guarantees or general throughput superiority over Redis.

## Active expiry index optimization

`active_expiry` now also exercises cached scheduling bounds: moving the first
TTL backward/forward in one hash stripe, zero and INT64_MAX timestamps, empty
binary keys, allocation rollback, four concurrent writers with a polling
reader, and a producer/consumer handoff between threads. The latter orders a
set on one thread before an erase on another, checking that the relaxed hint
cannot skip a published owner. Existing mutation, rejected-delete, EXEC,
replication, disk reopen and socket cleanup cases remain enabled.

The retained index uses a separate array of conservative deadline hints;
ordered/hash data access remains locked. Deletes/extensions may leave earlier
hints until a due poll refreshes them. This avoids shared timestamp writes on
every TTL mutation and preserves the original bucket layout. All new/changed
C++ lines are at most 80 columns and new helpers are under 200 lines.

`expiry_path_bench` is an optional diagnostic target, not a CTest. Build with
`PKV_BUILD_BENCHMARKS=ON`, then run `expiry_path_bench 100000 1`. MDB-only builds
omit oneTBB rows. The recorded C++11 Release experiment compares frozen before
and after binaries with both engines, five alternating paired trials and all
34 cases. Six design variants and every trial are archived: 2040 raw rows and
204 summaries. See [results and reproduction](../doc/EXPIRY_PERFORMANCE.md). Neither component rates nor noisy store cases
establish service throughput superiority over Redis.


The retained implementation passed **578/578 full-matrix CTests** after all
eight builds succeeded: memory 64, C++11 memory 64, oneTBB 71, C++11 oneTBB 71,
RocksDB Debug/Release 77 each, and WiredTiger Debug/C++11 Release 77 each.
All final build/test sessions reached terminal success. The two WiredTiger
suites ran separately after the other six completed. No timeout, expectation
or durability policy was weakened. Debug memory, oneTBB, RocksDB and WiredTiger
use ASan/UBSan; external libraries remain unsanitized.

Full logs are `/tmp/pkv-expiry-<build>-full.log`, detailed outputs are
`/tmp/pkv-expiry-<build>-full-details.log`, and build outputs are
`/tmp/pkv-expiry-<build>-build.log`. Counts, durations and log hashes are saved in
[validation metadata](../doc/benchmarks/expiry-validation-2026-10-31.json).
The final source hashes and all benchmark rows were checked against archived
metadata. An early shadowed local name failed the warning-as-error build and
was corrected before the retained implementation's matrix. No runtime failure
occurred in that final matrix. These checks do not prove race freedom,
power-loss safety, durable replication replay or complete Redis compatibility.

## SET value comparisons

`set_compare` checks native String IFEQ/IFNE without whole-value comparison
copies: 4-MiB values compare with a 64-KiB allocation ceiling; length mismatch
reads zero pages; a first-page mismatch reads one; GET comparison reads each
page once. Missing pages and rejected commits preserve data. Legacy no-ops
preserve bytes, successful native SET migrates legacy roots, and the legacy
JSON DAO also accepts these conditions. Allocation failure sweeps and four
threads completing 400 CAS increments exercise rollback and atomicity. oneTBB
configurations repeat the memory-engine cases with both implementations.

`set_compare_commands` runs real TCP comparisons with local Redis 8.6.2:
empty/binary/paged values, live/missing/expired keys, non-String errors,
GET/expiry options, duplicate/conflicting conditions, syntax errors, WATCH,
logical DBs and source EXEC. Socket races require exactly one CAS winner.
A separate primary/receiver fixture observes RPC readiness before checking a
source EXEC with one applied and one skipped comparison. It tests live delivery,
not initial synchronization or durable replay.

The local reference accepts some comparison-then-NX/XX combinations despite
the documented mutual exclusion. PKV rejects both orders. NUL-suffixed option
tokens are also rejected. These intentional differences have separate PKV
assertions and are excluded from identical-reply counts. The first differential
run exposed the order discrepancy; it remains documented rather than being
silently ignored. The implementation also clears a failed legacy SET result
instead of leaving an applied flag after storage rejection.

See [semantics, representation and limits](../doc/SET_COMPARISON.md).
Digest-based conditions and DIGEST/DELEX were added subsequently; see
[streaming digest coverage](../doc/STRING_DIGEST.md). No server throughput
superiority is inferred from these correctness checks.


The retained SET comparison implementation passed **594/594 full-matrix
CTests**: memory/C++11 memory 66 each, oneTBB/C++11 oneTBB 73 each, RocksDB
Debug/Release 79 each, and WiredTiger Debug/C++11 Release 79 each. Each build
passed 272 common protocol comparisons plus independent strictness, WATCH,
CAS-race and live source-EXEC replication checks. Both WiredTiger full suites
ran independently after the other six completed. Debug memory/oneTBB/RocksDB/
WiredTiger use ASan/UBSan; external libraries remain unsanitized. Every final
build/test session reached terminal success. No timeout or durability policy
was weakened, and no runtime failure occurred in the retained full matrix.

Full logs are `/tmp/pkv-cas-<build>-full.log`, detailed outputs are
`/tmp/pkv-cas-<build>-full-details.log`, and final build logs are
`/tmp/pkv-cas-<build>-final-build.log`. Source/reference identities, counts,
durations and log hashes are archived in
[validation metadata](../doc/benchmarks/set-comparison-validation-2026-10-31.json).
The original differential order mismatch remains in
`/tmp/pkv-cas-memory-target.log` and the focused diagnosis in
`/tmp/pkv-cas-memory-diagnostic.log`. Later common assertions use the documented
mutual exclusion; rejected comparison-then-NX/XX cases remain explicit separate
strictness assertions. New helpers/functions remain below 200 lines, and
changed implementation/test lines remain within 80 columns.

## String digests and conditional deletion

`string_digest` checks known XXH3-64 vectors, 21 length boundaries and five
stream chunk sizes. A 4-MiB native value is hashed/compared/deleted under a
64-KiB per-allocation ceiling; GET comparison hashes its existing reply and
reads each page once. It checks visited missing pages, rejected deletion,
owned-page removal, byte-preserving legacy reads/no-ops, the legacy JSON DAO,
allocation-failure rollback and eight-thread single-winner deletion. oneTBB
builds repeat applicable cases on both memory engines.

`digest_commands` compares DIGEST, SET IFDEQ/IFDNE and all DELEX conditions with
local modified Redis 8.6.2, including binary/empty/page boundary/integer values,
TTL composition, malformed lengths/nonhex tokens, absent/expired/non-String
error precedence, WATCH, source EXEC and logical DB selection. It also checks
single-winner socket deletion, argument-only dynamic DELEX key flags, cluster
routing and a live primary-to-receiver conditional update and physical removal.
RPC readiness is established before live delivery checks; initial sync and
durable replay are not inferred.

NUL-option and documented mutual-exclusion strictness remain explicit separate
assertions. The local reference emits an old-value frame followed by an error
for SET GET with a malformed digest; PKV returns one error and preserves data.
Standalone and EXEC checks require the following PING to receive PONG without
a leftover frame. The reference observation is recorded in
`/tmp/pkv-digest-reference-get-error.log`.

The first differential run found static DELEX key flags differing from Redis's
dynamic RM/RW flags; the implementation now follows its argument scan, including
malformed discovery requests. That failed run remains in
`/tmp/pkv-digest-memory-socket.log`. The repaired targeted two-test run passed
at `/tmp/pkv-digest-memory-fixed-target.log`, before later numeric, DB-isolation
and single-reply assertions expanded the fixture.

xxHash is a required external dependency (0.8.0+, tested with 0.8.3). Both
PKV_XXHASH_SOURCE_DIR mode and the installed-library branch were checked with
known one-shot and streaming vectors. A missing source directory produces an
explicit configuration failure. Source mode uses private PKV-prefixed symbols
and sanitizer instrumentation; external installed binaries retain their own
instrumentation. Setup and scope are documented in
[streaming String digests](../doc/STRING_DIGEST.md).


The retained implementation passed **610/610 full-matrix CTests**: memory /
C++11 memory 68 each, oneTBB / C++11 oneTBB 75 each, RocksDB Debug/Release 81
each, and WiredTiger Debug/C++11 Release 81 each. Every build passed 1524
common protocol assertions, plus separate strictness, single-reply framing,
WATCH, race, cluster and live replication checks. Both WiredTiger suites ran
independently after the other six completed. Every final build/test session
reached terminal success; no timeout or durability policy was weakened.
Debug memory/oneTBB/RocksDB/WiredTiger and their source-built xxHash use
ASan/UBSan. Other external libraries remain unsanitized.

Full logs are `/tmp/pkv-digest-<build>-full.log`, detailed outputs are
`/tmp/pkv-digest-<build>-full-details.log`, build logs are
`/tmp/pkv-digest-<build>-full-build.log`, and configure logs are
`/tmp/pkv-digest-<build>-config.log`. Counts, durations, final source/reference /
xxHash identities and log hashes are saved in
[validation metadata](../doc/benchmarks/string-digest-validation-2026-10-31.json).
The first attempt to build the new fixture target preceded explicit CMake
regeneration and reported an unknown target; reconfiguration resolved it.
The earlier metadata difference remains documented above. The retained full
matrix had no runtime failures. These results do not establish race freedom,
complete Redis parity, durable replay, initial synchronization or crash safety.

## Atomic MSETEX

`msetex` checks all-key NX/XX conditions, shared/preserved/past deadlines,
legacy JSON DAO behavior, and native import of live legacy records. Failed NX
preserves raw legacy bytes; successful XX KEEPTTL produces native PSR roots and
retains distinct old deadlines. Concurrent MGET reads must observe coherent
pairs during 200 four-thread assignments. Native/legacy allocation-failure
sweeps check both old payloads and per-key TTLs. A two-key, two-MiB assignment
runs under a 64-KiB per-allocation ceiling, with one native apply and bounded
pages. Rejected commits cannot report success or change TTLs.

`msetex_<backend>` checks native backend storage, independent disk reopening,
and values/deadlines after reopening. Two physical disk instances must reject a
mutating batch before any change, while an NX no-op across them returns 0.
`msetex_commands` compares full replies and subsequent values/types/deadlines
with local modified Redis 8.6.2. Cases include key populations and non-String
values, duplicate/binary/empty keys and values, all expiry modes, repeated and
conflicting options, invalid counts and malformed arguments, logical DBs,
source EXEC and argument-only command-key discovery.

Independent TCP assertions check shared relative deadlines against call-time
bounds, whole-batch NX single-winner races, WATCH no-ops/modifications including
past expiry, and cluster routing over only counted keys. Source/receiver checks
require coherent multi-key snapshots, preserved deadlines, source EXEC results
and background expiry physically removing both keys, observed via DBSIZE only.
Live RPC readiness is established first; these are not initial-sync/replay tests.

Strict NUL-option rejection and relative-expiry overflow remain separate
assertions. The first differential run found the local reference accepting
PX=LLONG_MAX after signed overflow; its failure remains in
`/tmp/pkv-msetex-memory-socket.log`. The boundary-adjusted run passed at
`/tmp/pkv-msetex-memory-boundary-target.log` with 499 common comparisons. PKV
rejects overflow before changing the old value/TTL, consistently with SET and
GETEX. This is an explicit compatibility difference, not an identical reply.

See [semantics, storage and physical-engine limits](../doc/MSETEX.md). No new
stored format, wire version or durability policy is introduced, and these
correctness checks do not establish server throughput superiority.

The initial six-configuration parallel run reached terminal failure in later
TCP fixtures: `EADDRNOTAVAIL` (Can't assign requested address), plus one broken
client barrier after connection setup failed. All new MSETEX fixtures had
already passed. The original full logs/details are retained as
`/tmp/pkv-msetex-<build>-first-full.log` and `-first-full-details.log`.
There were no ASan/UBSan error reports in those logs. An isolated retry of the
two failed memory fixtures passed at
`/tmp/pkv-msetex-memory-isolated-failed.log`. The later socket snapshot alone
does not prove a precise resource-exhaustion mechanism.

Final validation uses at most two independent configurations at once, followed
by isolated WiredTiger suites. Test expectations, internal client/thread/fiber
concurrency, timeouts and durability policy remain unchanged. No system TCP
setting was modified and no unrelated Redis process was stopped.


The final retained implementation passed **632/632 full-matrix CTests**:
memory/C++11 memory 70 each, oneTBB/C++11 oneTBB 78 each, RocksDB Debug/Release
84 each and WiredTiger Debug/C++11 Release 84 each. Every build passed 499
common TCP comparisons plus separate relative-deadline, strictness, WATCH,
race, cluster and live replication/physical-expiry assertions. Both WiredTiger
suites ran independently. Every final build/test session reached terminal
success with no changes to expectations, internal concurrency or durability.
Debug memory/oneTBB/RocksDB/WiredTiger use ASan/UBSan, including source-built
xxHash; other external libraries remain unsanitized.

Final logs are `/tmp/pkv-msetex-<build>-full.log`, detailed outputs are
`/tmp/pkv-msetex-<build>-full-details.log`, full build logs are
`/tmp/pkv-msetex-<build>-full-build.log` and the expanded legacy fixture rebuilds
are `/tmp/pkv-msetex-<build>-final-fixture-build.log`. Configure logs use
`/tmp/pkv-msetex-<build>-config.log`. Counts, durations, source/reference/log
hashes, final schedule and original network failures are archived in
[validation metadata](../doc/benchmarks/msetex-validation-2026-10-31.json).
New implementation/test lines are at most 80 columns and new functions remain
below 200 lines. These results do not prove complete Redis parity, race freedom,
initial synchronization, durable replay or crash safety.


## CLIENT metadata and RESET

`client_state` checks connection-owned IDs and metadata without a global client
registry. It covers every byte value, empty clears, self-alias replacement,
allocation failures preserving prior fields, allocation-free RESET, retained
library fields/ID and 8000 IDs allocated across eight threads. Copies are
forbidden so connection IDs cannot be duplicated accidentally.

`client_commands` compares 141 ordinary replies with local modified Redis
8.6.2, including SETNAME/GETNAME, SETINFO, arities, diagnostics, SELECT/RESET,
MULTI queue-time versus execution-time errors and command-key metadata. Separate
assertions exercise WATCH clearing, pipelined replies surviving RESET, unique
IDs across concurrent connections, strict embedded-NUL validation and cluster
routing of metadata-only and mixed transactions. The local redis-cli executable
also runs connection naming, SETINFO and RESET over the actual server socket.

CLIENT HELP advertises only ID, GETNAME, SETNAME, SETINFO and HELP. No CLIENT
INFO/LIST/KILL/PAUSE/TRACKING or HELLO/RESP3 support is implied. Library-field
retention after RESET is checked directly in C++ because public client-list
inspection is outside this change. See [session semantics](../doc/CLIENT_STATE.md).


The retained CLIENT/RESET implementation passed **648/648 full-matrix CTests**:
memory/C++11 memory 72 each, oneTBB/C++11 oneTBB 80 each, RocksDB Debug/Release
86 each and WiredTiger Debug/C++11 Release 86 each. Every configuration passed
141 common protocol comparisons and executed redis-cli integration. Both
WiredTiger suites ran independently; the other configurations ran in pairs.
No test expectations, internal concurrency, timeouts or durability settings
were weakened. Debug memory/oneTBB/RocksDB/WiredTiger use ASan/UBSan; other
external libraries remain unsanitized. No sanitizer errors were reported.

Full logs are `/tmp/pkv-client-<build>-full.log`, detailed outputs are
`/tmp/pkv-client-<build>-full-details.log` and build logs are
`/tmp/pkv-client-<build>-full-build.log`. All build/test sessions terminated
successfully. Source/reference/CLI/log hashes, counts, durations and scope are
archived in [validation metadata](
../doc/benchmarks/client-state-validation-2026-10-31.json).
New code lines are at most 80 columns and functions remain below 200 lines.
These correctness checks do not establish throughput superiority, whole-server
race freedom or complete Redis compatibility.


## HGETDEL

`hgetdel` verifies atomic, request-ordered field retrieval/removal on native
pages and explicit legacy JSON records. It covers missing/empty/binary data,
duplicates, draining root cleanup, retained TTL, parser rollback, corrupt/future
records, rejected commits/reads and an eight-thread single-winner race. Native
and JSON allocation-failure sweeps preserve every old field and the absolute
deadline at each failure, then verify the exact remaining fields on success.

The bounded fixture removes one small target from 64 unrelated 50-KiB fields
under a 32-KiB per-allocation ceiling. It requires zero unrelated blob reads and
one commit. This checks storage/work scaling, not server throughput. Native
backend fixtures exercise oneTBB/RocksDB/WiredTiger and independent disk reopen.

`hgetdel_commands` compares 412 ordinary replies/state checks with local modified
Redis 8.6.2, including randomized operations, type-before-syntax precedence,
count/arity errors, logical DBs, source EXEC and command metadata. HGETALL is
compared as an unordered map, while HGETDEL array order/nulls match exactly.
Separate checks cover strict NUL-option rejection, WATCH no-op/removal/draining,
an eight-client race, cluster field/key routing and live replication of native
source EXEC with a large removed field, retained TTL and physical root deletion.
See [semantics and limits](../doc/HGETDEL.md).

The first fixture attempt failed because its public expiry call migrated the
explicit JSON-only test record to PMR before calling the legacy DAO. The final
legacy fixture directly encodes its old-format deadline; the public native
path retains migration coverage. A three-field JSON allocation case reached
success after nine failure points; the final six-field case expands coverage
while retaining the original >10-point requirement. Initial diagnostics remain
in `/tmp/pkv-hgetdel-memory-target.log` and
`/tmp/pkv-hgetdel-memory-unit-count.log`. No production error behavior, expected
command replies, timeout or concurrency setting was relaxed for these fixes.


The final HGETDEL implementation passed **670/670 full-matrix CTests**:
memory/C++11 memory 74 each, oneTBB/C++11 oneTBB 83 each, RocksDB Debug/Release
89 each and WiredTiger Debug/C++11 Release 89 each. Every configuration passed
412 common TCP comparisons and the native/JSON allocation sweeps (77/12 failure
points, each reaching success). Other configurations ran in pairs and both
WiredTiger suites ran independently. Existing test expectations, internal
concurrency, timeouts and durability policies remain unchanged. Debug memory,
oneTBB, RocksDB and WiredTiger use ASan/UBSan; other external libraries remain
unsanitized. No sanitizer error reports appeared in the final logs.

All final build/test sessions terminated successfully. Logs are
`/tmp/pkv-hgetdel-<build>-full.log`, details are
`/tmp/pkv-hgetdel-<build>-full-details.log` and build logs are
`/tmp/pkv-hgetdel-<build>-full-build.log`. Source/reference/log hashes, per-build
counts/durations, schedule and initial fixture diagnostics are archived in
[validation metadata](../doc/benchmarks/hgetdel-validation-2026-10-31.json).
New implementation/test lines are at most 80 columns and functions remain below
200 lines. These correctness/structural checks do not prove complete Redis
compatibility, whole-server race freedom, crash safety or throughput superiority.


## HELLO and RESP3

`resp3_commands` compares 236 typed replies with local modified Redis 8.6.2,
covering native nulls/maps/sets/doubles, ordered pairs, LCS and leaf command
metadata. Optional metadata notes are excluded consistently. HELLO identity and
module contents are server-specific; PKV reports pkv/0.0.0 and no modules,
whereas the local reference includes built-in vectorset. Separate checks cover
protocol/error boundaries, default/no-password AUTH, unchanged state on failure,
strict NUL options, RESET, mixed-version pipelines/EXEC, WATCH abort nulls,
cluster no-slot negotiation and configured receiver role. Actual redis-cli -3
runs against both PKV and the reference, including RESP3 maps and a return to
RESP2. CLI raw map formatting is checked as `field value` on one line.

`response_cases.h` runs inside each existing C++11/C++17 array/vector cache RESP
fixture. It checks exact wire bytes, RESP2 fallbacks, binary pair lengths,
per-child protocol overrides, odd maps, invalid protocol values and cache reuse.
Existing parser/allocator fixtures remain enabled. `client_state` also verifies
protocol defaults/isolation and RESET; HELLO and CLIENT share the same exhaustive
metadata validator. See [protocol scope and remaining work](../doc/RESP3.md).

The legacy request parser still implements Redis bulk-array/inline command
framing. This change does not add arbitrary RESP3 request values, pushes,
Pub/Sub/tracking, streams, scripting, password configuration or ACL enforcement.
Only the existing unrestricted default/no-password user is available; HELLO
AUTH is not a new authorization boundary.


The first paired RocksDB Debug full run failed its `client_state` allocation
preservation assertion, and the initial isolated retry reproduced it. Adding
failure diagnostics and relinking passed; restoring the original test source
and relinking also passed, followed by 20 unchanged-source repetitions. No
production change or relaxed assertion/fault budget was applied. The precise
cause is not established, so this is not claimed as a diagnosed/fixed defect.
The final isolated RocksDB Debug full rerun passed all 90 tests. Initial logs are
`/tmp/pkv-resp3-rocks-first-full.log`, `-first-full-details.log` and
`/tmp/pkv-resp3-rocks-client-retry.log`; restored-source/repetition logs are
`/tmp/pkv-resp3-rocks-client-original.log` and `-client-repeat.log`.


The final retained per-configuration results passed **678/678 full CTests**:
memory/C++11 memory 75 each, oneTBB/C++11 oneTBB 84 each, RocksDB Debug/Release
90 each and WiredTiger Debug/C++11 Release 90 each. Each configuration passed
236 typed comparisons and actual redis-cli -3 integration against both servers.
The initial RocksDB Debug failure described above remains unexplained and is
retained alongside its unchanged-source repetitions and final isolated full
pass. Test concurrency, assertions, timeouts, fault budgets and durability were
not relaxed. Debug memory/oneTBB/RocksDB/WiredTiger use ASan/UBSan; other external
libraries remain unsanitized. ACL fiber stacks can produce ASan issue-189
warnings, so these results are not a claim of whole-server race freedom.

Full logs/details/builds use `/tmp/pkv-resp3-<build>-full.log`,
`-full-details.log` and `-full-build.log`. Counts, durations, source/reference/
CLI hashes, layout probe and the initial backend anomaly are archived in
[validation metadata](../doc/benchmarks/resp3-validation-2026-10-31.json).
The [GET comparison](../doc/RESP3_PERFORMANCE.md) retains both initial and longer
samples. The longer run checks 37,748,736 measured replies and observes median
rates within about one percent of the retained pre-change PKV baseline. It does
not compare against stock Redis or establish a general performance improvement.
New code lines are at most 80 columns and new functions remain below 200 lines.
These results do not establish complete Redis compatibility or crash safety.


## Default-user authentication and protected RPC

`password_policy` checks binary credentials across all 256 byte values,
first/middle/last mismatches, exact usernames and length/NUL boundaries. It
checks fragment bounds and one-MiB comparison with allocations disabled.
`client_state` now checks protected RESET and requires all three metadata
allocation failures to enter the replacement-new hook. A failure prints hook
counts/statuses and retains every prior unchanged-state assertion. Before this
change, 200 unchanged RocksDB fixture processes and a minimal three-hook probe
passed; the historical anomaly remains undiagnosed, not claimed as fixed.

`auth_commands` compares 72 ordinary replies with local modified Redis 8.6.2.
It checks protected and no-password modes, one/two-argument AUTH, error
precedence, failed attempts retaining prior login, DB/name/WATCH/transaction
state, RESET, HELLO AUTH and actual redis-cli -3 authentication. RPC cases reject
missing/wrong credentials and malformed lengths before any data, enforce an
incoming total deadline under trickled bytes, and authenticate versions 1/2/3.
A 4097-byte secret covers multi-fragment comparison and final-byte mismatch.
Live source EXEC/large-Hash replication uses both ordinary and long credentials;
wrong credentials and initially unavailable upstreams must leave the receiver
alive. Empty/closed RPC stream tests run in `rpc_transport`.

The first reference comparison exposed EXECABORT-wrapped NOAUTH and that reply
was implemented. Another reference case declares five EXEC children but emits
only four after wrong AUTH; raw evidence remains at
`/tmp/pkv-auth-reference-framing.log`. PKV instead requires one error per bad
AUTH/HELLO AUTH in pipelines and EXEC, with the next PING still aligned. These
are separate strictness assertions, not identical-reference claims.

An initial protected-receiver attempt crashed in ACL read after failed open/auth
closed the stream. The original ASan failure remains at
`/tmp/pkv-auth-memory-third-target.log`. Entry checks now reject null/closed
streams and the receiver reopens unopened streams. Negative-credential and
unavailable-source integration tests cover both paths. See
[configuration, security scope and remaining work](../doc/AUTH.md).


The final authentication full matrix passed **694/694 CTests**: memory/C++11
memory 77 each, oneTBB/C++11 oneTBB 86 each, RocksDB Debug/Release 92 each and
WiredTiger Debug/C++11 Release 92 each. Every build passed 72 ordinary AUTH
comparisons, 237 typed-protocol comparisons and real authenticated redis-cli
checks. Both WiredTiger full suites ran independently. All prior assertions,
fault budgets, internal concurrency, timeouts and durability settings remained
unchanged; metadata fault injection now additionally requires observed hooks.
Debug memory/oneTBB/RocksDB/WiredTiger use ASan/UBSan, while other external
libraries remain unsanitized. No sanitizer error reports occurred in final runs.

A final expression wrap fixes an 81-column line in master_service.cpp without
changing code tokens. All eight entry executables were rebuilt and auth_commands
rerun after that formatting-only change. Those supplemental logs use
`/tmp/pkv-auth-<build>-style-build.log`, `-style-target.log` and
`-style-details.log`. Original full-matrix logs use
`/tmp/pkv-auth-<build>-full.log`, `-full-details.log` and `-full-build.log`.
The validation metadata preserves the pre-wrap/full-suite source and final
source hashes, rebuilds and supplemental checks so the scopes remain explicit.

See [validation metadata](../doc/benchmarks/auth-validation-2026-10-31.json) and
[local GET measurements](../doc/AUTH_PERFORMANCE.md). The benchmark checks
37,748,736 replies with observed medians within about 1.5% of the retained PKV
baseline; it does not measure login cost or compare with stock Redis. The
historical OOM anomaly is still undiagnosed. New implementation/test lines stay
within 80 columns and functions below 200 lines. Per-user ACLs, TLS, password
rotation and full Redis compatibility remain unfinished.

## Connection registry lifecycle

`client_registry` checks consistent membership snapshots, duplicate/stale/foreign
record rejection, insertion/snapshot allocation failures, eight concurrent
membership workers, deferred/immediate stop transitions, 500 forced descriptor
reuse cases and 300 concurrent stop/retire/reuse races. The same standalone test
also runs under ThreadSanitizer; that run covers the registry, not the complete
ACL server.

`client_lifecycle` constructs actual ACL socket streams and Redis handlers. It
checks registration and retirement ordering, independent handler lifetimes and
that stopping a parsed pipeline prevents database writes. Management shutdown
keeps socket close ownership with ACL. Existing protocol/transaction/auth tests
continue to run with registration enabled. The initial registry-only revision advertised no public CLIENT INFO/LIST/KILL
command. CLIENT KILL is now covered by the extension below.

See [lifecycle scope](../doc/CLIENT_REGISTRY.md) and the
[validation report](../doc/benchmarks/client-registry-validation-2026-10-31.json).

The connection registry revision passed **710/710 full-matrix CTests**:
memory/C++11 memory 79 each, oneTBB/C++11 oneTBB 88 each, and RocksDB
Debug/Release plus WiredTiger Debug/C++11 94 each. WT configurations ran in
isolation. TSan registry and installed-ACL descriptor-retention probes passed
separately. Source fingerprints, binary hashes and full/detailed logs are in
the validation report above. The local checked GET comparison observes about
2.3% lower median RESP2 throughput than the retained previous build; see the
measurement limits in the registry scope document.


## CLIENT KILL

`client_kill_commands` compares 32 parameter and error cases with the isolated
reference and checks filter combinations, duplicate options, MAXAGE, pipeline
suppression, self/other shutdown, EXEC/DISCARD and runtime-error ordering.
It checks 32 concurrent killers, protected connections, RESP3, real redis-cli
and cluster routing. `client_kill_core` verifies execution while a different
thread owns exclusive KV admission and injects allocation failure before any
target shutdown. The registry fixture now verifies native IPv4/IPv6 addresses.

The reference's EXEC self-kill case commits remaining commands but can omit
declared array children. Its raw bytes are asserted separately; PKV must emit
the complete array before closing. See [semantics and evidence](
../doc/CLIENT_KILL.md). Full results and source hashes are retained in the
[validation report](../doc/benchmarks/client-kill-validation-2026-10-31.json).

The CLIENT KILL revision passed **726/726 full-matrix CTests**: memory/C++11
memory 81 each, oneTBB/C++11 oneTBB 90 each, and RocksDB Debug/Release plus
WiredTiger Debug/C++11 96 each. Existing concurrency, timeouts and durability
settings were preserved. An independent registry TSan run passed. The separate
checked GET comparison measured about 1.1% lower median RESP2 throughput than
the retained registry revision; no general speed conclusion is inferred.


## Client copied metadata and unordered capture

`client_snapshot` covers ordered/unordered membership, OOM output preservation,
atomic HELLO/RESET name/protocol views with four readers, and retained records
racing owner destruction. Ordinary reference getters remain actor-owned. The
new fixture and the existing registry fixture also pass independent TSan runs.
The complete revision passed **734/734 CTests**: memory/C++11 memory 82 each,
oneTBB/C++11 oneTBB 91 each, RocksDB Debug/Release 97 each and WiredTiger
Debug/C++11 97 each. Existing concurrency, durability and timeouts are preserved.

The optional `client_snapshot_bench` measures capture only. The 65,536-record
case observed about 2.18 ms sorted versus 0.289 ms unsorted. A separate checked
GET revision comparison observed about +1.1% median RESP2 throughput, without a
general speed or statistical-significance claim. See
[scope and reproduction](../doc/CLIENT_SNAPSHOTS.md) and the
[validation report](../doc/benchmarks/client-snapshot-validation-2026-10-31.json).
The following revision implements CLIENT INFO/LIST; complete resource telemetry
remains limited to the known-owned estimate described below.


## CLIENT INFO/LIST

`client_info_commands` compares 121 stable field/error assertions with the local
Redis reference. It checks ID order and duplicates, endpoint identity, dirty
WATCH state, idle MULTI/EXEC accounting, completed IO totals, partial request
memory, concurrent HELLO/LIST/disconnect, AUTH and RESP2/RESP3 framing. An 8 MiB
backpressure test requires observable retained output while another connection
can query the writer. Volatile allocation sizes are not compared with Redis.

`client_activity` and `client_input` cover single-writer counters, retirement
and real fragmented-parser/resource traversal. These fixtures and
`client_snapshot` also pass standalone TSan runs; the linked ACL archive is not
instrumented. `response_writer` exercises bounded write offsets above INT_MAX
without a multi-gigabyte allocation, and partial/closed/invalid write results.

See [field meanings and limits](../doc/CLIENT_INFO.md),
[allocation-failure investigation](../doc/CLIENT_INFO_OOM.md), and the
[validation report](../doc/benchmarks/client-info-validation-2026-10-31.json).


The CLIENT INFO/LIST revision passed **766/766 full-matrix CTests**: memory
and C++11 memory 86 each, oneTBB and C++11 oneTBB 95 each, RocksDB Debug/Release
and WiredTiger Debug/C++11 101 each. Debug configurations use ASan/UBSan.
WiredTiger configurations ran separately; other testing used at most two
configurations concurrently. Existing timeouts, concurrency assertions and
synchronous disk settings were preserved. Three standalone TSan fixtures and
20 final-path metadata OOM repetitions also passed. The retained older
link-dependent OOM anomaly has no proven platform-wide root cause.


## Named ACL users and permissions

`acl_policy` verifies SHA-256 standard vectors, immutable policy publication,
password alternatives, rule staging, selectors, command categories/subcommands,
read/write key patterns, quoted rule replay and identity generations. It
injects allocation failure across 128 existing-user budgets, 64 new-user
budgets, eight HELLO metadata budgets and removal staging. Four policy readers
race 2000 complete policy replacements. The standalone TSan policy fixture
instruments these paths; copied client metadata/parser fixtures are also rerun.

`acl_commands` compares 72 common assertions against the local reference,
then checks binary password hashes against hashlib, complete HELLO identity
changes, multi-key authorization, queued/EXEC revocation, same-name user reuse,
KILL USER, RESP3 shapes, OS-backed GENPASS and eight clients racing 128 policy
changes. A protected key must retain its original value throughout that race.
`resp3_commands` now compares 251 typed assertions: the ACL parent fields and
all ten advertised children are checked exactly against matching reference
entries, excluding only reference-only LOAD/SAVE/LOG children from that parent
comparison. Existing parent exclusions and child checks remain in place.

See [permissions and limits](../doc/ACL_PERMISSIONS.md).


The ACL runtime revision passed **782 full-matrix CTests**: memory/C++11 memory
88 each, oneTBB/C++11 oneTBB 97 each, RocksDB Debug/Release and WiredTiger
Debug/C++11 103 each. That retained candidate has 69 ACL differential assertions.
The final binary adds focused denied-key diagnostic and dynamic default-login
corrections, validated by **40 follow-up CTests**: native ACL, socket ACL,
RESP3, AUTH and CLIENT KILL admission in each of eight configurations. The
final socket fixture has 72 differential assertions plus independent transition,
revocation, crypto/randomness and concurrency checks. Native policy/metadata/
parser TSan fixtures passed independently (four total). No existing timeout,
concurrency assertion or disk synchronization setting was relaxed.

The final fixture also toggles default on/nopass for an existing unauthenticated
connection, requires actual authentication for HELLO, and requires reauthentication
and EXECABORT after a password is restored. Both PKV and the reference execute
these transitions. Source/binary fingerprints and separate full/focused logs
are retained in [the validation report](
../doc/benchmarks/acl-validation-2026-10-31.json).


## ACL file persistence

`acl_file` covers staged reloads, stable/revoked identities, stale plans,
category intent, quoted replay, 512 allocation budgets and zero-allocation
commit. Four readers race 2000 reloads. File faults cover opens, ENOSPC,
short/zero/overreported writes, EINTR, fsync, close, rename and parent fsync.
The fixture verifies original-file preservation, owned-temp cleanup, 0600
mode, close-on-exec descriptors and bounded writes.

`acl_file_commands` verifies explicit SAVE/LOAD, RESP2/RESP3, startup failures,
restarts, password hashes, retained OFF sessions, removed-user disconnection,
default fallback and self-removal inside EXEC. Common cases run on the local
modified Redis 8.6.2 reference. A separate unmatched-selector probe crashes
that reference; PKV's corresponding rejection remains mandatory.

`resp3_commands` now checks 253 typed assertions, including all twelve
advertised ACL children. Only unsupported ACL LOG is excluded from the
reference parent comparison. Prior revision counts above are historical.
See [file semantics and evidence](../doc/ACL_PERSISTENCE.md).


The ACL persistence revision passed **798/798 full-matrix CTests**: memory
and C++11 memory 90 each, oneTBB and C++11 oneTBB 99 each, RocksDB
Debug/Release and WiredTiger Debug/C++11 105 each. Debug uses ASan/UBSan;
WiredTiger runs were isolated. Two current standalone TSan fixtures cover
policy and reload publication. Existing timeouts, concurrency assertions
and synchronous disk settings were preserved. A new child-list line was
wrapped within 80 columns after the first six configurations; AST equality
confirms identical fixture semantics. Source/binary fingerprints, archived
logs, file fault/OOM coverage and the separate reference-crash evidence are
in [the validation report](
../doc/benchmarks/acl-file-validation-2026-11-01.json).


## ACL LOG security events

`acl_audit` verifies grouping at the 60000/60001-ms boundary, newest-ten scan
limits, promotion, latest client metadata, binary fields, capacity eviction,
RESET ID continuity, disable/reenable and delayed worker timestamps. Thirty-two
allocation budgets verify staged new/coalesced records. Four readers race eight
writers producing 8000 exact-count events, followed by reset/capacity races.
The standalone TSan fixture instruments this core without the fiber runtime.
A retained timestamp variant fails the delayed-writer regression probe.

`acl_log_commands` compares 18 stable observations with the local modified
Redis 8.6.2 reference, plus typed RESP3 maps/double values, failed AUTH/HELLO,
command/subcommand/key denials, binary keys, queue/EXEC contexts, DRYRUN and
NOAUTH/syntax exclusions. It tests default 128-entry eviction and ten-entry
queries, custom capacity, disabled retention and negative startup rejection.
Eight clients produce 800 denials that must aggregate without losing counts.
`resp3_commands` now includes all thirteen ACL children and 254 typed assertions.
See [audit semantics and limits](../doc/ACL_AUDIT.md).


The core candidate passed **814 full-matrix CTests**: memory/C++11 memory
92 each, oneTBB/C++11 oneTBB 101 each, RocksDB Debug/Release and WiredTiger
Debug/C++11 107 each. A final revoked-identity diagnostic correction passed
**56 focused CTests** in those same eight configurations. Native policy and
socket self-removal probes fail before the correction and pass afterward;
subsequent denied EXEC children retain their canonical command names in LOG.
Two current standalone TSan fixtures cover audit and final policy behavior.
Full and focused phases are distinguished in
[the validation report](../doc/benchmarks/acl-log-validation-2026-11-01.json).
A final native test entry-line wrap has identical Clang C++11 preprocessed
tokens. Existing timeouts, concurrency assertions and disk sync were preserved.


## Runtime CONFIG foundation

`configuration` tests complete-plan validation, mutable/immutable parameters,
case/glob semantics, binary passwords, unchanged-value behavior and preservation
of default-user OFF/permission state. It injects 32 preparation budgets and
512 publication budgets; a failure must preserve both password and audit limit.
The private RPC startup password must remain unchanged by frontend updates.

`configuration_commands` compares 31 stable observations with the local modified
Redis 8.6.2 reference. It checks multi-pattern deduplication and field spelling,
errors, multi-parameter rollback, runtime authentication, ACL child permissions,
RESP2/RESP3 and EXEC sequencing. Four preconnected readers each make 150 queries
while 100 password/limit pairs are changed; every pair must be coherent.
Unsupported Redis persistence activation must fail without updating credentials.
The audit fixture also covers deferred runtime limits, including coalescing,
distinct-event trimming, disabled cleanup, ID continuity and concurrent setters.
`resp3_commands` now has 257 typed comparisons and exactly checks all advertised
CONFIG GET/SET/HELP child rows against the reference.

See [configuration semantics and limitations](../doc/CONFIGURATION.md).


The CONFIG foundation passed **830/830 full-matrix CTests**: memory/C++11
memory 94 each, oneTBB/C++11 oneTBB 103 each, RocksDB Debug/Release and
WiredTiger Debug/C++11 109 each. Debug builds use ASan/UBSan. The native audit
TSan fixture covers the new deferred-limit operations and races. All 69 source
fingerprints were fixed during the complete matrix. Existing timeouts,
concurrency assertions and disk synchronization settings were preserved.
See [the retained validation evidence](
../doc/benchmarks/config-validation-2026-11-01.json).

## INFO and RESETSTAT

`server_statistics` covers worker-local accounting, 32 allocation budgets for
atomic reset publication, error-prefix allocation failure and 80,000 concurrent
updates with a snapshot reader. `expiry_snapshot` covers bounded TTL sampling,
signed/unsigned timestamp extremes and lazy discovery in a 65,536-DB pool,
including an unavailable stored database.

`info_commands` compares 13 stable statistics observations against the local
modified Redis 8.6.2 reference. It covers queued/executed/rejected/failed calls,
ACL changes during MULTI, reset scope and reset inside EXEC. It also checks
section selection, identity stability, actual memory/CPU fields, logical root
counts, RESP3 verbatim replies, eight concurrent clients with exactly 800 PINGs,
network bytes and pipeline reset behavior. Disk variants restart an existing
nonzero database and query INFO before SELECT. They then replace the stored DB
directory with a regular file: INFO must return ERR, and PING must still work
on that connection.

See [field sources and concurrency limits](../doc/INFO.md). These tests extend
the prior CONFIG suite; they do not establish complete Redis compatibility.

The final INFO increment passed **854/854 full-matrix CTests** on macOS ARM64:
memory/C++11 memory 97 each, oneTBB/C++11 oneTBB 106 each, and RocksDB
Debug/Release plus WiredTiger Debug/C++11 112 each. Debug builds use ASan/UBSan.
The native statistics TSan fixture also passed. The final 87 source fingerprints
stayed fixed throughout this matrix; original timeouts, concurrency and disk
sync settings were retained. RESP3 now has 259 typed comparisons.
See [the validation evidence](
../doc/benchmarks/info-validation-2026-11-01.json) and
[the scoped GET benchmark](../doc/benchmarks/info-get-2026-11-01.json).

## Cached command admission and lookup

The catalog fixture additionally checks lowercase and alternating-case lookup
of every registered root command, rejects all 128 high-byte suffixes per root,
and repeats the checks with four concurrent readers. Existing socket suites
exercise cached administrative admission, standalone CLIENT control, exclusive
EXEC and keyspace reads, and null-handler transaction state transitions.

The dispatch revision passed **854/854 full-matrix CTests** on macOS ARM64 with
the same eight configurations and unchanged test/durability settings. All 89
recorded source fingerprints stayed fixed. Debug builds used ASan/UBSan.
The separate checked MDB/oneTBB Release measurements cover 113,246,208 measured
GET requests with matched per-backend flags and same-protocol baselines.
See [dispatch rules and measurement limits](../doc/COMMAND_DISPATCH.md) and
[the validation report](../doc/benchmarks/dispatch-validation-2026-11-01.json).

## Native HyperLogLog

`hyperloglog` covers sparse/dense codecs, every dense register position,
randomized six-bit values, sparse promotion, cardinality/cache behavior, native
operations and key extraction. Legacy JSON tests preserve source bytes on union
reads, preserve WATCH during encoding-only import, and invalidate it on updates.

`hyperloglog_fault` injects 128 allocation budgets each into PFADD, PFCOUNT and
PFMERGE. Failure must preserve roots, pages and WATCH. It also rejects storage
commits explicitly. A separate 4 MiB fake sparse String runs under a 64 KiB
individual-allocation cap: cached count, empty add and invalid-body paths must
each read only the first native page. oneTBB builds repeat the allocation cases
against both memory backends.

`hyperloglog_commands` compares 191 stable observations with the local modified
Redis 8.6.2 reference, including exact raw sparse/dense bytes and cardinalities,
binary inputs/keys, malformed and noncanonical data, import, cache state and
complete command descriptors. Additional cases check TTL, WATCH, ACL key access,
MULTI/EXEC, logical DB selection, cluster cross-slot errors and keyless INFO in
EXEC. Eight clients concurrently add 128 elements each; final dense bytes and
cardinality must equal the reference union. Disk variants restart DB 3. Private
RPC checks include a replica's local PFCOUNT cache update followed by primary
updates and a grouped PFADD/PFMERGE EXEC.

RESP3 adds typed HLL command replies and now has 271 comparisons. Metadata tests
retain and compare optional key-spec notes. See [implementation scope and
storage bounds](../doc/HYPERLOGLOG.md).

The final HLL revision passed **878/878 full-matrix CTests** on macOS ARM64:
memory/C++11 memory 100 each, oneTBB/C++11 oneTBB 109 each, RocksDB Debug/Release
and WiredTiger Debug/C++11 115 each. Debug configurations used ASan/UBSan. All
106 source fingerprints stayed fixed during the final matrix. Existing
timeouts, concurrency checks and disk sync settings were preserved.

The post-HLL GET measurements checked 100,663,296 responses across matched MDB
and oneTBB Release binaries with same-protocol baselines. See
[the HLL validation evidence](
../doc/benchmarks/hll-validation-2026-11-01.json) for frozen source/binary/log
fingerprints, allocation outcomes, protocol comparisons and measured limits.

## Inline String reads

`inline_string` validates MDB/oneTBB bounded root reads, zero heap allocations
with reserved output, allocation failure, expiry cleanup, WATCH preservation,
legacy and malformed fallback, paged values and workspace read-your-writes.
Eight readers race 1200 inline/paged writes, replication overwrites and deletes
per backend. See [read path scope](../doc/INLINE_STRING_READS.md).

## GEO core commands

`geo` checks score encoding, 100,000 coordinate round trips, distance rounding,
inclusive bounds, arbitrary ZSet scores, legacy import and native 16 KiB row
limits with a 40,000-byte member. `geo_commands` covers exact reference replies,
typed RESP2/RESP3 values, atomic invalid-input rejection, NX/XX/CH, TTL, WATCH,
ACL @geo/key modes, EXEC, concurrent inserts, restart and private RPC replay.
See [GEO storage and command scope](../doc/GEO.md).

## Indexed GEO search and replica write admission

`geo_search` covers zero-distance/grid regressions, indexed local/ANY reads,
bounded top-k allocations, 40,000-byte members and 512 allocation budgets per
memory backend. `geo_search_commands` covers exact reference search/store
results, protocols, options, WATCH/ACL/TTL/EXEC, restart and grouped replay.
`replica_readonly_commands` checks all 84 supported write roots, live CONFIG
aliases, startup, transaction admission and watch cleanup against a reference
replica. See [GEO search](../doc/GEO_SEARCH.md) and
[replica policy](../doc/REPLICA_READONLY.md).

## Legacy GEO radius commands

`geo_radius_keys` covers keyword-based key specs, first-match metadata, actual
target admission, reverse/dangling scans and shared pointers.
`geo_radius_commands` covers reference results, repeated stores, protocol
types, missing-source parsing, TTL/WATCH/ACL, DB 3 EXEC, cluster, restart and
grouped private RPC. PKV-specific assertions protect effective targets beyond
the public metadata list. See [radius scope](../doc/GEO_RADIUS.md).

## Blocking lists and connection input

`key_wait` checks FIFO, generations, cancellation and cross-thread/fiber
handoff. `list_ready` checks typed producers, expiry, overlapping reservations,
4096-key allocation-free publication and 128 storage fault budgets. Optional
backend variants exercise real oneTBB, RocksDB and WiredTiger storage.
`blocking_io` checks fragmented input, quotas, startup/shutdown, weak control
ownership and 32 startup fault budgets. `execution_gate` checks nested shared
suspension, restoration and exclusive isolation.

`blocking_list_commands` checks five blocking commands and CLIENT UNBLOCK,
429 stable/20 typed differential observations, FIFO overlaps, early pipeline
replies, deferred input, disconnect/kill, ACL/replica rechecks, WATCH, DBs,
cluster errors, restart, grouped private RPC and six command-statistics cases.
See [blocking semantics and scope](../doc/BLOCKING.md).

## Blocking sorted sets

`zset_ready` checks bounded native pages, ordered workspace/private RPC
commits, List/ZSet notification isolation and 512 allocation budgets while
both subscription types are active, plus 128 deferred partial-write budgets.
It also checks 4096-key mixed grants and per-fiber batch ownership. Optional
backend variants exercise real
oneTBB, RocksDB and WiredTiger storage. `blocking_zset_commands` checks 288
stable and 18 typed differential observations, FIFO overlaps, cancellation
after type changes, mixed List/ZSet waiters and EXEC order, global waiting
types, CONFIG/cancellation inside EXEC, producers including STORE/GEO,
cross-DB COPY, pipelines, ACL/replica rechecks, WATCH, restart and grouped RPC.
See [blocking ZSet scope](../doc/BLOCKING_ZSET.md).

## Queue indexes and heap ownership

`key_wait_heap` checks 1536 allocation budgets across all waiting types,
including existing subscribers, and requires no outstanding C++ allocations.
`key_wait_index` checks binary/duplicate keys, arbitrary removals, per-type
FIFO reuse and overlapping grants with allocation disabled. The optional
`key_wait_bench` target measures standalone queue scaling; it is not a socket
or storage throughput test. See [queue measurements](../doc/QUEUE_SCALING.md).

## Pub/Sub output transport foundation

`connection_output` checks reply barriers, external versus self pushes,
4,000 queued and 16,384 concurrently consumed cross-thread frames, binary
socket output, slow-reader closure, active-frame quotas, retained capacity,
partial/chunked writes, allocation failures and concurrent shutdown. The
transport was initially validated before command integration. See
[ownership and ordering](../doc/PUBSUB_OUTPUT.md)
and the [eight-configuration validation report](
../doc/benchmarks/output-validation-2026-11-01.json). The report retains one
unexplained startup failure on the unchanged RocksDB Release server alongside
its passing full rerun and targeted reproduction checks.


## Pub/Sub broker and commands

`pubsub` checks ordinary/pattern/shard indexes, binary frames, duplicate and
empty-family acknowledgements, protocol switching, self-publication order,
selector-union validation, foreign endpoints and quotas. It injects 1,152
allocation budgets and checks rollback, and verifies shared encoding of a
1 MiB publication for 128 recipients using only two protocol frames.

`pubsub_commands` checks typed socket replies, RESP2 restrictions, RESP3
blocking subscribers, pipelines and EXEC, cross-DB publication, CLIENT/INFO,
pattern sharing, read-only replicas, cluster routing, concurrent publishers,
subscribe/unsubscribe boundaries and slow-reader cleanup. ACL scenarios cover
same-user policy updates after AUTH, unrelated users, selector diagnostics,
self-close replies and ACL LOAD. Fourteen independent checks reject NUL
command/subcommand suffixes, whose reference behavior varies by hash seed.
See [command semantics and limits](../doc/PUBSUB.md).

`pubsub_handler` injects 64 one-shot allocation failures into the actual first
SUBSCRIBE handler. Connections that recover without a subscription must still
return a complete 33 MiB ECHO reply. This covers output promotion, startup,
rollback and normal-mode quotas, including failures before the first successful
subscription. Subscription acknowledgements transfer their encoded buffer into
a response object reserved before membership changes.

On Darwin arm64, the installed ACL fcontext library lacks ASan stack-switch
registration. Deep exception unwinding then reports poisoned alternate-stack
regions. Use the matching ACL source and archive to build a private test copy:

```sh
python3 tools/build_acl_fiber_asan.py \
    --acl-source /path/to/acl \
    --library /path/to/libfiber.a \
    --output /tmp/pkv-acl-asan
cmake -S . -B /tmp/pkv-asan-build \
    -DPKV_TEST_SANITIZERS=ON \
    -DFIBER_LIBRARY=/tmp/pkv-acl-asan/libfiber.a
```

The tool currently supports Darwin arm64 private fcontext stacks only. It
checks source anchors, changes a copied archive, records input/output hashes,
and never writes the installed library or ACL checkout. The source tree must
match that archive. It registers first entry, resumption, thread-stack bounds,
reused fibers and final exit using the sanitizer fiber API, preserving fake
stacks for use-after-return checks. It does not suppress reports or disable
sanitizers. A separate intentional stack-overflow probe still triggers ASan.
Release configurations use the original archive. See the validation report for
both the initial diagnostic and the complete registered-stack test runs.

The startup probe also handles a listener closing before configuration
validation completes. It retries transport closure until the child exit is
observed, while malformed frames and incorrect AUTH/PING replies still fail.
This avoids treating an expected negative-configuration rejection as an
unrelated BrokenPipe failure.

The final Pub/Sub revision passed 1,056 full CTests across eight configurations
and five additional repetitions of each of four focused fixtures. See the
[final Pub/Sub evidence](../doc/benchmarks/pubsub-validation-2026-11-01.json)
for source/binary/runtime fingerprints, fault budgets, startup-probe regression
checks and matched-protocol GET measurements. These results do not establish
complete Redis semantics or a throughput advantage over official Redis.


## Random key selection and command time

`key_sampling` checks rank bijections, binary/empty names, stale/error indexes,
workspace shadowing, collision buckets, allocation-free memory selection,
96 allocation budgets, no payload IO and concurrent atomic transfers. It also
checks expired private-page reclamation and failed cleanup preserving pages
and WATCH. Backend fixtures reopen persistent data in independent processes
and repeat selection across multiple storage instances.

`random_key_time_commands` covers RESP2/RESP3 types, all value families, passive
expiry, key-name discovery ACLs, Pub/Sub restrictions, cluster and read-only
admission, mixed EXEC changes and concurrent writers. `execution_gate` now
checks that frozen microsecond and millisecond clocks agree and unfreeze at
the end of exclusive admission.

The optional `key_sampling_bench` target compares native rank selection with a
streaming visitor on the same memory backend, using identical random rank
sequences and matching sequence checksums. It is a raw index microbenchmark,
not server throughput or a Redis comparison. See [costs and scope](
../doc/KEY_SAMPLING.md).

The RANDOMKEY/TIME revision passed 1,078 full CTests across eight configurations
and five additional repetitions of each of four focused fixtures. Each backend
runs 636 behavioral checks; all-command RESP3 checks now total 327. The
[validation record](../doc/benchmarks/sampling-validation-2026-11-01.json)
identifies 565 frozen sources, native/disk fixtures, runtime archives, unchanged
codecs, raw-index measurements and matched-protocol GET samples. This does not
establish constant-time disk sampling or complete Redis compatibility.


## Dense memory root indexes

`dense_index` compares SCAN membership, root counts and every deterministic rank
through growth, arbitrary deletion and reinsertion. It covers stable pointer
positions, cancellation, allocation-free erasure and warmed small-array reuse,
192 allocation budgets per memory backend, failed direct/batch writes and
concurrent colliding writers. MDB exercises 41 failed/151 successful budgets;
oneTBB exercises 53 failed/139 successful budgets.

The dense-index revision passed 1,086 CTests across eight configurations and
20 focused repetitions using 569 frozen source files. The
[validation record](../doc/benchmarks/dense-validation-2026-11-01.json) includes
source/runtime hashes, unchanged storage/codec proofs and raw benchmark hashes.
`tools/benchmark_key_updates.py` compares matched Release revisions for hot
root overwrites, insert/delete churn and retained fills; every reply and final
root count is checked. Resident memory uses only the owned server PID and
includes allocator/runtime/cache memory. See
[design, measurements and limits](../doc/DENSE_KEY_INDEX.md).


## Paired page preparation

`page_pair` transfers binary-named roots with 16-KiB pages between MDB/oneTBB
pairs, including mixed engines. Each pair covers 320 allocation budgets across
empty/populated destinations. Checks include original roots/pages/TTL on
failure, replacement-family cleanup, counts/ranks, WATCH and generic wake hints,
explicit failure before/after the second preparation, allocation rejection after
both MDB preparations, no-op/abort/poisoned transactions and native read failure.
Observer probes require both native publications and WATCH effects to complete
before either callback. Concurrent reverse transfers retain one physical root
in coherent snapshots. Same-DB delegation and unsupported disk/workspace
capabilities have direct checks; disk rejection is checked before storage opens.

This revision passed 1,094 CTests across eight configurations and 25 focused
repetitions on 573 frozen source files. See the
[validation report](../doc/benchmarks/pair-validation-2026-11-01.json) and
[scope/performance](../doc/PREPARED_PAGE_COMMITS.md). These tests establish an
internal memory coordination boundary, not public MOVE support or cross-disk
crash atomicity. Current command inventory and unsupported work remain explicit.


## Native commit scratch buffers

`native_commit_buffers` uses a private isolated MDB instance and thread-local
allocation counters. A native overwrite of one existing 128-byte root with no
page deltas performs four allocations; erasing that root performs two. It
verifies successful root bytes/counts, and excludes command/codec/network or
observer allocation. Existing page/pair/dense fixtures retain rollback fault
coverage, including new owner bookkeeping reserved before insertion.

The lazy-buffer revision passed 1,102 CTests across eight configurations and
30 focused repetitions, with 574 frozen source files and 571 unchanged parent
sources. See [validation](../doc/benchmarks/lazy-validation-2026-11-01.json)
and [allocation scope/benchmarks](../doc/NATIVE_COMMIT_BUFFERS.md). Matched
GET/write comparisons and the predeclared three-variant screen retain original
samples, compiler/runtime fingerprints and explicit performance limits.


## Physical WAL barriers

`durability` checks sync status/exception propagation, volatile and actor-local
rejection, completed-initialization gating, value/TTL/WATCH/observer/wake
preservation and same-key ordering with independent-stripe progress. Native disk
helpers exercise binary/empty keys, 16-KiB private pages, single/multiple stores,
reopen, indexes and barriers on deleted/missing owners. A valid first path plus
a file as the second path must fail initialization and reject the barrier.
Each `durability_crash_rdb/wdb` fixture kills six acknowledged epoch writers
and two acknowledged deletion writers, then verifies independent recovery.
It kills only the fixture's own child processes in temporary directories.

The revision passed 1,118 CTests across eight configurations and 47 focused
repetitions on 581 frozen sources. See
[validation](../doc/benchmarks/sync-validation-2026-11-01.json) and
[contracts and limits](../doc/DURABILITY_BARRIERS.md). These checks do not
simulate hardware power loss or inject every native IO failure; the fake error
probe covers the PKV boundary. The barrier alone is not a cross-DB transaction.


## MOVE decision journal

`move_journal` tests the internal decision record, not the Redis MOVE command.
It covers binary/empty/large keys, every-byte truncation/corruption, length and
configuration checks, native lock ownership, permissions and close-on-exec,
short/zero/overreported IO, EINTR, sync/close/rename/removal failures, explicit
persistence order and stale serials. Two 48-budget loops cover publication and
startup allocation failures. `move_journal_crash` kills only owned writers at
ten persistence/acknowledgment cutpoints, independently decodes pending native
records with Python struct/zlib, then reopens twice at every cutpoint.

All eight configurations passed 1,134 CTests; 590 source fingerprints are kept.
Five further repetitions of both journal fixtures on each of four ASan builds
added 40 passes. See [validation](
../doc/benchmarks/journal-validation-2026-11-01.json) and
[journal contracts and unimplemented coordinator work](../doc/MOVE_RECOVERY.md).
Process-kill coverage does not simulate power loss or verify native DB MOVE
application. Public MOVE and grouped cross-DB replication still need
end-to-end implementation and tests. See the subsequent startup checks below.


## Native MOVE redo and WT positioned reads

`move_recovery_rdb/wdb` uses real native DBs and a real decision journal. It
checks bounded one-page commits, an 8-MiB String, binary/empty keys, both physical
partitions, expired absolute deadlines, target orphan removal, page/root/index
identity and restart. Hash/List/Set/ZSet cases use 48-KiB blobs or members, then
read and mutate recovered data without damaging it. Eight owned process-kill
cutpoints replay/check twice; seventeen injected failures include errors after
native commits and must emit the requested marker before SIGABRT. Five corrupt
pending cases repeatedly abort with the same journal. Sanitizer errors are
rejected even when child termination is expected.

`wt_read_errors` injects native cursor getter failures only after real successful
positioning, checks 21 failure cases and genuine absence, then restores normal
reads. Getter failure cannot become a missing-key/end result. Cursor/session
function pointers are restored before native close, all in a private fixture DB.

The initial eight-profile targeted selection passed 298 CTests on 599 sources.
After the cursor probe found a missed error condition, both WT profiles were
rebuilt and passed 42 checks each, including the new unit. The final 600-source
state and initial/corrected test scopes are recorded in
[validation](../doc/benchmarks/redo-validation-2026-11-01.json) and
[contracts](../doc/NATIVE_MOVE_REDO.md). Server policy/effects integration,
grouped replication and public MOVE remain unfinished.


## Bound native startup recovery

`move_domain` tests persistent identity, ownership, corruption, partial binding,
new-marker write failures and existing-marker read/sync failures.
`move_startup_mdb/tbb` verifies volatile rejection and ordinary default startup.
`move_startup_rdb/wdb` starts the real server with a committed native decision
across two roots. Five interrupted/expired states recover before the first
PONG; seven rejected configurations preserve control bytes and recover after
correction. Two competing-process cases preserve the existing default/enabled
owner and verify the losing first activation writes no binding marker.

This startup revision passed 362 selected CTests across eight configurations
on 607 frozen code/test/build files. This is a targeted selection, not the
entire CTest suite.

See [startup contract](../doc/MOVE_STARTUP.md) and
[scoped matrix](../doc/benchmarks/startup-validation-2026-11-01.json). The matrix
uses C++11 Release and C++17 ASan/UBSan, the established private fiber runtime
for sanitizer builds, and isolated sequential WT tests. No power-loss, online
MOVE, standard Redis replication or new throughput result is claimed.


## Retained MOVE storage coordinator

`move_storage` checks predecision errors, callback requirements, capability and
ID rejection, serial overflow, source-before-decision WAL ordering, continuous
exclusion of both writer threads and completion under locks. Its stable-key
regression changes the caller's string in preparation and verifies the original
owner still moves. An immutable ASan before/after probe uses identical source.

`move_storage_rdb/wdb` checks genuine 8-MiB String and Hash/List/Set/ZSet families,
CRC/root/page/index identity, large blobs, expired-target policy and later
mutation. Native counters check two source fences and one target fence. Eight
owned interruption points and sixteen failure paths reopen/check twice. The
failed-unlink-sync case distinguishes absent journal from completed data.

These tests exercise the physical component in isolated DBs, not a public
MOVE handler or a complete logical-effects/replication adapter. See
[contract](../doc/MOVE_STORAGE.md) and
[validation](../doc/benchmarks/coordinator-validation-2026-11-01.json).

The coordinator revision passed 374 selected CTests across eight configurations
on 614 frozen code/test/build files. This is a targeted selection, not the
entire CTest suite. The independent same-source ASan key probe fails with the
pre-fix core and passes with the corrected core. Initial fixture failure,
corrected fixture scope and the later key fix retain separate fingerprints.


## Memory page-family ownership

`move_memory` checks two prepared native engines, allocation-failure rollback,
zero-allocation physical publication, unchanged payload addresses and CRCs,
constant transfer allocations from 1 to 2048 pages, and forward/reverse DB IDs.
Hash/List/Set/ZSet payloads include large blobs or members and later mutation.
It also checks WATCH, two waiting writers, paired generic/List readiness and
active expiry without a scan/rebuild. Oversized roots must be rejected before
copying; unsupported pairs must not call policy.

`move_memory_crash` launches owned child processes in both native frame orders.
Exceptions at validation/preparation must return with original data; exceptions
at physical publication/completion must abort rather than return an ordinary
rollback result. No malformed metadata-only target event is emitted.

See [ownership contract](../doc/MEMORY_MOVE_FAMILIES.md) and
[scoped validation](../doc/benchmarks/family-validation-2026-11-01.json).

Final memory-family validation records 390 broad selected passes and 60 final
selected passes across eight configurations, with separate source fingerprints.
The final extension covers empty/binary inline values and empty/binary keys
without pages, in both frame orders and both target replacement modes.
These selections are not the full CTest suite.

## Memory cross-DB group replay

`replica_group` covers allocation/native preparation rollback across DBs,
ordered-prefix validation, maintenance/WATCH history, local readiness/expiry,
wire commit boundaries, mixed MDB/oneTBB and iterative 64-DB preparation.
See [contracts and scope](../doc/CROSS_DB_REPLAY.md) and the linked validation
report for source/build/runtime fingerprints and exact test selections.

Final cross-DB group validation completed eight all-target builds and 356
selected checks (37/37/44/44/48/48/49/49). The report preserves two intermediate
TCP expectation corrections and their source states separately from the final
passing matrix. Production/C++ unit sources did not change during those fixes.

## Replica read retention

`replica_retention` checks five paged types, byte/address preservation, delayed
partial updates plus PERSIST, read-only EXEC, legacy/cache reads and actor-local
scope isolation. `replica_retention_stream` repeats the delayed String case with
real TCP servers and asserts no read-induced downstream mutation. CMake passes
the configured MDB/oneTBB/RocksDB/WiredTiger backend to the stream fixture.
See [contracts and evidence](../doc/REPLICA_READ_RETENTION.md).

The final retention source state passed 458 selected CTests across eight
all-target build configurations: MDB 49/49, oneTBB 57/57, RocksDB 61/61 and
WiredTiger 62/62 (Debug/Release). Four Debug builds use ASan/UBSan; this does
not represent the full CTest suite. Exact test names and fingerprints are in
[retention validation](../doc/benchmarks/retention-validation-2026-11-01.json).

The subsequent [disabled-guard optimization](
../doc/READ_GUARD_FAST_PATH.md) passed the same 458 selected CTests after all
eight all-target rebuilds. Existing actor-scope and delayed-replay fixtures
retain their expectations; only two execution-gate implementation files changed.

## Atomic memory page-family plans

`family_plan` checks family permutations, explicit patches, 1024 allocation
budgets per memory engine, native failure ordinals, large typed values,
readiness ranks, WATCH and publication under exclusive admission.
`family_plan_crash` checks pre-publication rollback and post-publication abort.
See [contracts and regression evidence](../doc/ATOMIC_FAMILY_PLANS.md).

The final family-plan source state passed 474 selected CTests across eight
all-target builds: MDB 51/51, oneTBB 59/59, RocksDB 63/63 and WiredTiger 64/64
(Debug/Release). Four Debug builds use ASan/UBSan. This is not a full CTest run;
see [family-plan validation](
../doc/benchmarks/family-plan-validation-2026-11-01.json) for exact scope and
the intermediate lifetime failure plus its identical-object correction.

## Ordered ownership replay view

`family_workspace` stages complete authoritative page/MOVE histories under
exclusive admission, retained until destruction. It checks prefix reads,
cycles, source recreation, latest page deltas, WATCH history, current-waiter
List/ZSet ranks, maintenance and expired data followed by PERSIST. Each memory
engine exercises 1024 allocation budgets plus native failure ordinals.
Typed String/Hash/List/Set/ZSet cases consume actual handler-generated page
batches; they do not implement or test a new MOVE wire protocol.

`family_workspace_crash` reuses the publication-boundary subprocess driver.
The many-destination fixture guards against quadratic native rollback-array
reservation; the retained identical-object probe verifies the allocation change.
The 1/64/4097-page fixtures check unchanged allocation addresses, zero page
visits/reads/copies and family-size-independent C++ allocation counts.

Final validation completed eight all-target builds and 490 selected CTests:
MDB 53/53, oneTBB 61/61, RocksDB 65/65 and WiredTiger 66/66 (Debug/Release).
Four Debug configurations use ASan/UBSan. This is not a full CTest run.
See [contracts and measurements](../doc/OWNERSHIP_REPLAY_VIEW.md) and
[exact selections and evidence](
../doc/benchmarks/ownership-validation-2026-11-01.json).

## Normal process shutdown

`server_exit` requires successful SIGTERM/SIGINT exit with expiry off/on and a
blocked client. Disk backends reopen the same storage and compare large paged
String/Hash/List/Set/ZSet values in logical DBs 0 and 7. The shared integration
fixture also rejects abnormal exit or forced termination after every server
context; passing command assertions alone no longer hide a teardown crash.

`database_shutdown` injects close failures/exceptions, verifies all stores are
attempted, successful close is idempotent, lazy creation is disabled and command
admission drains before shutdown. The six-case same-input signal probe fails
with SIGABRT against the unchanged binary and exits normally after the fix.

Final validation built all targets in eight configurations and passed 506
selected CTests: MDB 55/55, oneTBB 63/63, RocksDB 67/67 and WiredTiger 68/68.
Four Debug builds use ASan/UBSan. This is not the complete CTest suite. The
initial RocksDB dependency RTTI link failure is retained separately from the
successful resumed checks; no sanitizer checks were disabled.
See [shutdown contract and scope](../doc/PROCESS_SHUTDOWN.md),
[validation evidence](../doc/benchmarks/shutdown-validation-2026-11-02.json)
and [stages with remaining estimates](../doc/ROADMAP_STATUS.md).

`server_callback_exit` additionally exercises ACL idle/use limits and the real
POSIX daemon supervisor status-channel disconnect. Each has expiry off/on,
with/without a replica receiver blocked on an owned upstream socket. Supervisor
cases retain a connected client through shutdown. All require successful exit
and explicit native-close completion. Disk cases reopen the same data and
compare five paged types in DBs 0 and 7. `wt_clean_exit_check` opens both WT homes
with recovery forbidden before PKV restarts, exposing unclosed native stores
that ordinary restart recovery could hide. The test rejects the old executable;
the native checker also rejects the old replica stores that need recovery.
See [callback shutdown scope](../doc/CALLBACK_SHUTDOWN.md).

The callback validation matrix built all targets in eight configurations and
passed 514 selected CTests. A subsequent combined-backend registration guard
was checked by regenerating/rebuilding all eight configurations with identical
binaries, flags and selected test definitions, plus two configure-only combined
registrations. Both source versions are recorded in the
[callback validation report](../doc/benchmarks/callback-shutdown-validation-2026-11-02.json).

## Early startup cancellation

`startup_signal` checks real configuration and ACL FIFO readers with SIGTERM
and SIGINT, both with ordinary output and a completely full stderr pipe left
undrained. Each cancellation must return zero within two seconds. A linked
component helper distinguishes pre-publication cancellation from an enabled
callback (test exit seven), and throwing/returning callbacks must exit one.
Native cases reopen five large paged types in DBs 0 and 7 after interrupting
an opened store, and restart the actual server after interrupting MOVE recovery
at page-copy, target-publication and source-deletion cutpoints. These helper
cutpoints do not insert test hooks into the production server.

The final eight-configuration matrix passed 540 selected CTests, including
224 startup scenarios; four Debug builds use ASan/UBSan. Earlier focus runs and
the matrix stopped to fix stderr backpressure are recorded separately. The
existing `move_startup_*` fixtures now require rejected startup status one and
normal stop status zero, with no successful forced-kill teardown.
See [startup contract](../doc/STARTUP_CANCELLATION.md) and
[validation evidence](../doc/benchmarks/startup-cancellation-validation-2026-11-02.json).
