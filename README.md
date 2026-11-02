# pkv
Parallel KV service supports redis protocol.

# Features
- **Multiple store engines:** Supports rocksdb, wiredtiger, Intel tbb hashmap, stl hashmap, acl hash table;
- **Multiple threads:** Use multiple CPU cores with multiple threads;
- **Coroutine IO:** Use the coroutine lib from acl;
- **Streamed parser:** Streamed parser which is IO independent; 
- **Serialization:** Bounded typed pages for String/List/Hash/Set/ZSet, with
  codec-independent legacy views. The default yyjson codec reads old records;
  native expiry and key transfers share the typed migration path. See
  [storage format](doc/STORAGE_FORMAT.md) and
  [legacy expiry migration](doc/LEGACY_EXPIRY.md) and
  [key transfers](doc/LEGACY_TRANSFERS.md).

# Supporting redis commands
- **Connection:** AUTH, HELLO (RESP2/RESP3), PING, ECHO, QUIT, RESET; CLIENT ID/GETNAME/SETNAME/SETINFO/HELP
- **Database:** DBSIZE; [counting and recovery](doc/DBSIZE.md)
- **Discovery:** COMMAND, COUNT, INFO, LIST, GETKEYS, GETKEYSANDFLAGS,
  HELP and basic DOCS; see [metadata coverage](doc/COMMAND_METADATA.md)
- **Key:** KEYS (binary-safe glob), TYPE, EXISTS, DEL, RENAME/RENAMENX, COPY (DB/REPLACE), EXPIRE/PEXPIRE, EXPIREAT/PEXPIREAT, TTL/PTTL, EXPIRETIME/PEXPIRETIME, PERSIST; expiry conditions NX/XX/GT/LT; stateless SCAN with MATCH/COUNT/TYPE on the `mdb` backend
- **String:** LCS (LEN/IDX/MINMATCHLEN/WITHMATCHLEN), GET, DIGEST, DELEX, MGET,
  MSET, MSETNX, MSETEX, SET with NX/XX/IFEQ/IFNE/IFDEQ/IFDNE, GET, KEEPTTL,
  EX/PX/EXAT/PXAT; INCR/DECR/INCRBY/DECRBY, APPEND, STRLEN, GETRANGE/SUBSTR,
  SETRANGE, GETDEL, GETEX, GETSET, SETNX, SETEX/PSETEX; binary keys and values
  on the `mdb` backend
- **Hash:** HSET, HGET, HDEL, HGETDEL, HMSET, HMGET, HGETALL, HSCAN (MATCH/COUNT/NOVALUES), HSETNX, HINCRBY, HLEN, HSTRLEN, HEXISTS, HKEYS, HVALS; binary fields/values, ordered nil results, per-key atomic modifications, and TTL retention on the tested `mdb` backend
- **List:** LPUSH/RPUSH, LPUSHX/RPUSHX, LPOP/RPOP (optional count), LLEN, LRANGE, LINDEX, LSET, LTRIM, LINSERT, LREM, LPOS (RANK/COUNT/MAXLEN), LMOVE/RPOPLPUSH, LMPOP; atomic mutations, binary elements, TTL retention, and key removal when drained
- **Set:** SADD, SREM, SCARD, SISMEMBER, SMISMEMBER, SMEMBERS, SSCAN (MATCH/COUNT), SMOVE, SPOP, SRANDMEMBER, SINTERCARD (LIMIT), SUNION/SINTER/SDIFF and their STORE variants; binary members, TTL retention and coherent multi-key snapshots

- **Sorted Set:** ZADD (NX/XX/GT/LT/CH/INCR), ZINCRBY, ZREM, ZCARD, ZSCORE/ZMSCORE, ZRANK/ZREVRANK (WITHSCORE), ZRANGE/ZRANGESTORE (rank/BYSCORE/BYLEX, REV, LIMIT), legacy range aliases, ZCOUNT/ZLEXCOUNT, ZREMRANGEBYRANK/BYSCORE/BYLEX, ZPOPMIN/ZPOPMAX, ZMPOP (MIN/MAX, COUNT), ZRANDMEMBER (optional count and WITHSCORES), ZSCAN (MATCH/COUNT), ZUNION/ZINTER/ZDIFF and STORE variants, ZINTERCARD (LIMIT); see [remaining options and storage limits](doc/SORTED_SETS.md)

List, Hash, Set and ZSet use bounded native pages on `mdb`, `mdb_tbb` and
single-instance `rdb`/`wdb`, with
atomic JSON migration, page-aware TTL/COPY/RENAME and PKV2 replication. Other
engines currently reject Hash/List/Set/ZSet requests until they implement native page
transactions. ZSet uses coordinated member and score indexes. See
[bounded List storage](doc/LIST_STORAGE.md) and
[bounded Hash storage](doc/MAP_STORAGE.md) and
[bounded Set storage](doc/SET_STORAGE.md) and
[bounded ZSet storage](doc/ZSET_STORAGE.md).

SELECT uses connection-local logical databases (`databases = 16` by default),
separate from physical `db_count` shards. Negotiated version 1 live replication
carries DB identity; legacy subscribers retain DB 0 only. See
[logical DB/COPY behavior and limits](doc/LOGICAL_DATABASES.md).

The custom DB 0 replication stream now validates empty/binary-key frames and
preserves same-key publication order across workers; [replication limits](doc/REPLICATION.md)
still include initial synchronization, reliable replay and replica roles.
PKV3 adds whole-EXEC framing with memory multi-DB coordination and a
single-DB disk apply domain; see
[EXEC replication](doc/EXEC_REPLICATION.md).
PKV4 adds bounded ordered page/MOVE transport and memory receiver replay; see
[ordered replication](doc/ORDERED_REPLICATION.md). Public
[MOVE](doc/MOVE.md) now connects the native coordinator and prepared
[source capture](doc/MOVE_SOURCE_CAPTURE.md); durable disk mixed groups,
initial synchronization and PSYNC remain pending.

macOS startup now initializes ACL event hooks before concurrent schedulers.
See [diagnosis and validation](doc/FIBER_STARTUP.md) and the
[public MOVE validation](doc/benchmarks/public-move-validation-2026-11-02.json).

Command presence does not imply complete Redis compatibility. See
[compatibility status and roadmap](doc/COMPATIBILITY.md) for remaining work.

See [scan semantics and implementation limits](doc/SCAN.md) before relying on
COUNT as a performance bound or enabling another backend.

The native `mdb`/`mdb_tbb` and single-instance `rdb`/`wdb` backends provide
atomic multi-key MSET/MSETNX/DEL and coherent MGET snapshots. Cross-physical-store
mutations remain restricted. See [batch guarantees](doc/ATOMIC_BATCH.md) and its
cluster migration limits before changing an existing deployment.

# Dependence
- **c++11**
- **CentOS7.6 or Ubuntu 18**
- **Acl project:** https://github.com/acl-dev/acl
- **Yyjson:** https://github.com/ibireme/yyjson
- **fpconv:** bundled for score formatting under Boost Software License 1.0; see [origin and license](third_party/fpconv/ORIGIN.md)
- **Rocksdb:** https://github.com/facebook/rocksdb
- **Wiredtiger:** https://github.com/wiredtiger/wiredtiger
- **Intel oneTBB:** https://github.com/oneapi-src/oneTBB

# Build
## Build and install acl
In the acl source path run below as root:
```shell
 # make & make packinstall
```
Then the libs including libacl_all.a, libfiber_cpp.a and libfiber.a will be installed to `/usr/lib`, and the header files of acl will be installed in `/usr/include/acl-lib`.

## Build and install yyjson, rocksdb, wiredtiger, oneTBB
In the source path of every project run:
```shell
# mkdir build
# cd build
# cmake ..
# make -j 4
# make install
```

## Build pkv

Requires CMake 3.10+, C++17 (or C++11), ACL, yyjson and xxHash 0.8.0+.
Memory backends are
built by default. RocksDB, WiredTiger, oneTBB, and jemalloc are opt-in:

```shell
make
# Optional backends, after installing their development libraries:
make HAS_ROCKSDB=YES HAS_WT=YES HAS_TBB=YES HAS_JEMALLOC=YES
# C++11:
make BUILD_WITH_C11=YES
```

The binary is `build/pkv`. Build artifacts remain in the build directory.
CMake respects `CMAKE_PREFIX_PATH` for dependency discovery. For a local yyjson
checkout, compile it directly without installing it:

```shell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DPKV_YYJSON_SOURCE_DIR=/path/to/yyjson \
  -DPKV_XXHASH_SOURCE_DIR=/path/to/xxHash
cmake --build build -j 4
```

For xxHash, install its development header/library or compile a local checkout:

```shell
cmake -S . -B build -DPKV_XXHASH_SOURCE_DIR=/path/to/xxHash
```

Source mode builds a private namespaced library; it does not modify that
checkout or require a system installation. Both source directory options can
be passed together. `XXHASH_INCLUDE_DIR` and `XXHASH_LIBRARY` select an installed
library explicitly. xxHash is needed for Redis-compatible DIGEST and digest
conditions; no upstream algorithm is copied or reformatted in PKV sources.
Pass the same `-D` options through `make CMAKE_ARGS="..."` when using Make.
For persistent per-machine options, create `Makefile.local`, which Git ignores:

```make
CMAKE_ARGS += -DPKV_YYJSON_SOURCE_DIR="/path/to/yyjson"
CMAKE_ARGS += -DPKV_XXHASH_SOURCE_DIR="/path/to/xxHash"
```

Plain `make` then uses those options, including after a new build directory is
created. Command-line Make variables retain precedence over this local file.
An explicitly enabled backend with missing headers or libraries fails during
configuration with a dependency-specific error. Use a new build directory when
switching from an older cached configuration if it still enables optional libraries.

# Test
Run regression tests with `ctest --test-dir build --output-on-failure`.
The full suite needs permission to start temporary loopback TCP listeners.
See [test instructions](tests/README.md) for sanitizers and Redis comparison.

Start the default memory build with `./build/pkv alone mdb.cf`.
The sample configurations listen on port 19001. Configurations selecting an
optional backend require that backend to be enabled at build time.

- Run pkv with data being stored memory by oneTBB:
```shell
# ./build/pkv alone mdb_tbb.cf
```

- Run pkv with data being stored memory by hash table of acl:
```shell
# ./build/pkv alone mdb_htable.cf
```

- Run pkv with data being stored in rocksdb:
```shell
# ./build/pkv alone rdb.cf
```

- Run pkv with data being stoared in wiredtiger:
```shell
# ./build/pkv alone wdb.cf
```

- Run the redis-benchmark in redis to test the pkv's performance
```shell
# ./redis-benchmark -p 19001 -n 10000000 -P 1000 -t set -r 100000000
```

## Native memory storage

`mdb` and `mdb_tbb` store Hash, List, Set and ZSet as bounded native pages, with atomic
multi-key edits and page replication. The oneTBB backend uses short-lived reader
accessors and preserves command locks for same-key/multi-key atomicity. It is
not an entirely lock-free execution path. See [storage details](doc/TBB_STORAGE.md).
Cross-path disk transactions and complete Redis compatibility remain under
development; see [coverage](doc/COMPATIBILITY.md).

The [native ZSet dual-index implementation](doc/ZSET_STORAGE.md) now backs the
server's existing ZSet commands, including atomic legacy migration, scan/key
operations and PKV2 replication.

## RocksDB native page mode

Build with `HAS_ROCKSDB=ON` to enable this backend. RocksDB stores
List/Hash/Set/ZSet roots and owned pages in separate column
families with one atomic WriteBatch per physical instance. Existing default-CF
keys remain readable. Use the new single-instance example for this mode:

```sh
./build/pkv alone rdb_native.cf
```

`rdb_native.cf` uses a fresh `./data/rdb-native` path. Existing four-shard
`rdb.cf` paths are preserved; cross-physical-shard mutations are rejected before
writing until distributed atomic recovery is implemented. WAL reopen/process-kill
recovery is tested; normal writes retain the prior non-fsync default. See
[RocksDB layout, recovery and limits](doc/ROCKSDB_STORAGE.md).


## WiredTiger native page mode

Build with `HAS_WT=ON` and run `./build/pkv alone wdb_native.cf` for a fresh
single-path native database. Roots, bounded pages and SCAN entries commit in
one snapshot transaction with synchronous logging. Binary/empty keys survive
reopen; readable legacy `table:access` records are imported without modifying
the old table. Independent-path mutations are rejected before writing. See
[WiredTiger migration, recovery and limits](doc/WIREDTIGER_STORAGE.md).


`HRANDFIELD` supports distinct/repeated field samples and `WITHVALUES` through
native page ranks, without materializing a whole Hash for a small sample.
See [Hash sampling and validation](doc/HASH_SAMPLING.md).


`INCRBYFLOAT` and `HINCRBYFLOAT` preserve key TTL and publish final computed
values to replication. Hash increments update bounded field pages and skip
oversized numeric value fragments. See [floating increments](doc/FLOAT_INCREMENTS.md).


Existing String commands now use [native String storage](doc/STRING_PAGES.md):
small values stay inline; large values use bounded chunks. Ranges, APPEND and
SETRANGE access their relevant pages; TYPE/TTL/COPY/RENAME and PKV2 understand
page ownership. Legacy JSON values migrate on single-key access, while MGET
remains read-only.

The [String DAO microbenchmark](doc/STRING_STORAGE_BENCHMARK.md) compares native
chunks with the old JSON path; it is not a Redis throughput comparison.

GETBIT/SETBIT and BITCOUNT/BITPOS now use native String pages, including
BYTE/BIT ranges and atomic updates in shared bytes. See [bitmaps](doc/BITMAPS.md).

BITOP supports all eight operators using atomic page output, source aliases and
read-only legacy inputs. See [BITOP design and validation](doc/BITOP.md).

BITFIELD and BITFIELD_RO support native signed/unsigned fields, overflow
policies and atomic compound updates. See [field semantics](doc/BITFIELD.md).

MULTI/EXEC/DISCARD and WATCH/UNWATCH now provide in-process transaction
isolation and optimistic concurrency. PKV3 replicates whole EXEC groups and
applies single-database groups atomically. Eligible source EXEC groups now
commit once through a native workspace. Cross-database/physical-store recovery
and remaining legacy fallbacks are still open. See
[source commits](doc/SOURCE_EXEC.md), [transactions](doc/TRANSACTIONS.md) and
[EXEC replication](doc/EXEC_REPLICATION.md).

LCS reads two coherent native/legacy String snapshots and uses compact
traceback storage. See [semantics and memory bounds](doc/LCS.md).

KEYS now enumerates one coherent local keyspace, including staged EXEC writes.
SCAN and WATCH share native metadata inspection without an unused payload view.
See [keyspace behavior and limits](doc/KEYSPACE.md).

Primary services now perform indexed [active expiration](doc/ACTIVE_EXPIRY.md),
with locked deadline rechecks, bounded work quanta and atomic native page
reclamation. Startup/repair snapshots and memory costs are documented there.
Atomic scheduling bounds avoid idle bucket locking; see
[local expiry-index measurements and limits](doc/EXPIRY_PERFORMANCE.md).

SET also supports atomic IFEQ/IFNE value conditions, using page-sized comparison
reads and reusing GET's old value. See
[value comparisons and compatibility limits](doc/SET_COMPARISON.md).


DIGEST, SET IFDEQ/IFDNE and DELEX now use streaming XXH3 and atomic conditional
updates/deletions. See [semantics and dependency setup](doc/STRING_DIGEST.md).


MSETEX now assigns counted key/value pairs with shared expiry, whole-batch
NX/XX conditions and per-key KEEPTTL. See
[atomic batch semantics and limits](doc/MSETEX.md).


Connection metadata and RESET now support client setup, unique connection IDs,
names and library attributes, with transaction/WATCH/DB reset behavior. See
[supported CLIENT commands and session scope](doc/CLIENT_STATE.md).

HGETDEL now retrieves and removes requested Hash fields in one native page
transaction. See [atomic field retrieval, storage and limits](doc/HGETDEL.md).

HELLO now negotiates typed RESP3 replies, including maps, sets, doubles and
per-command protocol changes in pipelines/EXEC. See [protocol scope](doc/RESP3.md).

Optional requirepass/masterauth now protects Redis commands and companion RPC
subscriptions. See [default-user authentication and limits](doc/AUTH.md).

Public connections now use a striped lifecycle registry, with retirement before
ACL closes sockets and protection against stale descriptor reuse. See
[registry scope](doc/CLIENT_REGISTRY.md). CLIENT KILL now supports cross-thread
connection closure and filters; see [semantics](doc/CLIENT_KILL.md). CLIENT
INFO/LIST now report connection state; see [scope](doc/CLIENT_INFO.md).


KILL now captures unordered membership in linear time. Connection metadata can
be copied safely across workers, including atomic HELLO name/protocol updates
and protection against destroyed handler state. CLIENT INFO/LIST now use
these views for connection diagnostics. See [snapshot scope and measurements](doc/CLIENT_SNAPSHOTS.md).


CLIENT INFO/LIST now provide protocol-aware connection diagnostics, ID/type
filters, live byte/command counts, WATCH/MULTI state and actual parser/output
resource measurements. Allocation fields have documented PKV meanings. See
[semantics, measurements and limits](doc/CLIENT_INFO.md).


Named ACL users now authenticate through AUTH/HELLO and enforce command,
category, subcommand, selector and key read/write permissions, including MULTI
and EXEC rechecks. CLIENT diagnostics and KILL USER use actual user identities.
Startup `aclfile`, ACL SAVE and ACL LOAD now persist users with staged
replacement and atomic file saves. ACL LOG now reports bounded recent
authentication and permission failures with RESP2/RESP3 output; frontend ACL
changes do not replace the separate RPC startup password policy. See
[runtime permission semantics](doc/ACL_PERMISSIONS.md),
[file persistence and failure behavior](doc/ACL_PERSISTENCE.md), and
[security event logging](doc/ACL_AUDIT.md).


CONFIG GET now reads actual running values with multiple patterns and
deduplicated RESP2/RESP3 results. CONFIG SET validates complete multi-parameter
changes before applying frontend requirepass and audit capacity updates.
Unsupported Redis RDB/AOF activation fails explicitly. See
[configuration scope, atomicity and validation](doc/CONFIGURATION.md).

INFO now reports runtime process, connection, command/error, network, memory,
CPU and logical keyspace statistics. CONFIG RESETSTAT resets cumulative
command/connection/network/error counters while retaining ACL denial totals.
See [statistics sources, concurrency and limits](doc/INFO.md).

Command dispatch now caches administrative admission categories and uses ASCII
case folding for registry lookup. Ordinary KV commands skip control-command
comparisons while retaining exact statistics. See
[dispatch rules and matched MDB/oneTBB measurements](doc/COMMAND_DISPATCH.md).

PFADD, PFCOUNT and PFMERGE now use native paged String transactions with Redis
sparse/dense HLL interoperability, bounded sketch processing and cardinality
cache semantics. See [HyperLogLog storage and command scope](doc/HYPERLOGLOG.md).

MDB and oneTBB GET now read valid inline String roots without constructing a
page transaction, retaining key locking and expiry/type validation. See
[inline String read scope and validation](doc/INLINE_STRING_READS.md).

GEOADD, GEOPOS, GEOHASH and GEODIST now share the bounded native ZSet format,
with Redis-compatible coordinates, geohashes and distance replies. See
[GEO command scope, storage and validation](doc/GEO.md).

GEOSEARCH and GEOSEARCHSTORE now query bounded score-index ranges, with
COUNT ANY early exit and bounded top-k retention. See [indexed GEO search](
doc/GEO_SEARCH.md). Replicas now reject client writes by default, with
[explicit writable-mode configuration](doc/REPLICA_READONLY.md).

The four GEORADIUS variants now share indexed GEO search and atomic bounded
stores. The ten @geo command names match the reference inventory; repeated
store options receive explicit effective-target admission checks. See
[radius commands and key semantics](doc/GEO_RADIUS.md).

BLPOP, BRPOP, BLMPOP, BLMOVE and BRPOPLPUSH now use native paged Lists,
with CLIENT UNBLOCK, disconnect cancellation and FIFO wake handling. See
[blocking list semantics and validation](doc/BLOCKING.md).

BZPOPMIN, BZPOPMAX and BZMPOP now share the blocking connection machinery,
with type-filtered shared readiness and native ZSet indexes. Cancellation probes
also preserve waiting clients after a key changes type. See
[blocking ZSet semantics and validation](doc/BLOCKING_ZSET.md).

Waiting queues now cache per-type FIFO chains and release every node when
subscription allocation fails. Same-key queue scaling and matched revision
measurements are documented in [waiting queue scaling](doc/QUEUE_SCALING.md).


The nine Pub/Sub command names now support binary-safe local delivery across
worker threads, RESP2/RESP3 frames, channel ACLs, shared message fanout and
bounded slow-client output. Shard routing, live client diagnostics and EXEC
reply order are covered by differential tests. Cross-node publication remains
unfinished; see [Pub/Sub semantics and scope](doc/PUBSUB.md).


RANDOMKEY now samples the selected database's live root keys, including binary
and empty names, without reading native payload pages. Memory backends use
[dense root indexes](doc/DENSE_KEY_INDEX.md); disk/workspace paths stream keys. TIME returns the
microsecond wall-clock snapshot, shared by all commands in one EXEC. See
[semantics, costs and verification](doc/KEY_SAMPLING.md).


Memory page commits now have a reversible preparation boundary, with an internal
coordinator that prepares both databases before publishing either. This is
[groundwork for atomic cross-database writes](doc/PREPARED_PAGE_COMMITS.md);
the public [MOVE adapter](doc/MOVE.md) now uses it with ordered publication.


MDB native commits now allocate scratch buffers on first use. Existing-root
writes and deletions avoid unused rollback buffers while retaining atomic
preparation and bounded pages. See [allocation contracts and measurements](
doc/NATIVE_COMMIT_BUFFERS.md).


An internal owner-scoped WAL barrier now synchronizes the selected RocksDB or
WiredTiger storage instance without changing ordinary write options or logical
values. Volatile/workspace and failed-initialization cases decline explicitly.
See [durability contracts, recovery checks and MOVE limits](
doc/DURABILITY_BARRIERS.md).


An internal, checksummed single-decision MOVE journal now has native lock,
IO-failure and process-kill recovery coverage. It stores DB IDs and a binary key,
without serializing collection values. The public adapter now binds this
journal for online native transfers. See
[MOVE journal contracts and validation](doc/MOVE_RECOVERY.md).


Internal native MOVE redo now transfers bounded private pages for RocksDB and
WiredTiger, preserves root bytes and absolute expiry, and replays interrupted
target-before-source application. A native cursor fault probe also led to a WT
read/scan error-classification fix. See
[contracts and scoped validation](doc/NATIVE_MOVE_REDO.md). Default-auto
[bound startup recovery](doc/MOVE_STARTUP.md) now runs before client handling,
expiry and RPC workers, with stable storage identity and fail-closed validation.
The public [MOVE command](doc/MOVE.md) uses this recovery domain; disk
multi-command replication recovery remains unfinished.


An internal [retained native MOVE storage coordinator](doc/MOVE_STORAGE.md)
now holds both key locks from validation through the source WAL fence,
durable decision, bounded application and completion. Startup uses the same
physical application phase. An owned key snapshot prevents callbacks changing
a caller's string from redirecting the committed operation. Public MOVE
connects server policy, paired logical effects and prepared ordered publication.


Internal [memory page-family ownership](doc/MEMORY_MOVE_FAMILIES.md) now hands
MDB/oneTBB page tables across DBs without copying their pages. Both engines
prepare before publication, preserving rollback and bounded metadata. Local
WATCH, expiry and readiness effects precede the required observer callback.
The public command now publishes these transfers through the PKV4 stream.


[Memory cross-DB replay](doc/CROSS_DB_REPLAY.md) now prepares all received native frames
before publishing any. The private v3 receiver supports MDB/oneTBB groups;
PKV4 adds public MOVE publication and memory ownership replay. Durable disk
mixed groups remain unfinished. Single-DB replay retains its existing path.


[Replica read retention](doc/REPLICA_READ_RETENTION.md) prevents expired client reads from
erasing pages needed by delayed primary deltas. Logical reads still report
missing data; authoritative replay and explicit writes keep their native paths.

[Disabled replica-read guard fast path](doc/READ_GUARD_FAST_PATH.md) removes
external guard calls from the disabled path while preserving actor-local
replica read retention.

[Atomic memory page-family plans](doc/ATOMIC_FAMILY_PLANS.md) prepare native
family permutations and explicit page patches together. This internal
prerequisite now underlies the MOVE-aware PKV4 receiver workspace.
