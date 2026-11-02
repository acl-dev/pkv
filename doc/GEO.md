# GEO commands and bounded storage

GEOADD, GEOPOS, GEOHASH and GEODIST use the existing PZR dual-index ZSet.
GEOADD validates every coordinate before the one atomic ZSet mutation; an
invalid later triple leaves earlier triples unpublished. NX, XX and CH use
ordinary ZADD update semantics, including repeated members and no-op WATCH
preservation. Existing TTL survives updates. Missing or expired XX targets
remain absent. GEO keys remain ordinary ZSets, interoperable with ZSCORE,
ZMSCORE, ZRANGE, ZREM, key transfers and existing persistence/replication.

Coordinates use Morton-interleaved integer scores, with 26 bits per axis and
Redis's inclusive upper-coordinate endpoint behavior. The latitude range is
[-85.05112878, 85.05112878]; longitude is [-180, 180]. Decoding returns the
quantization cell center, clamped to those bounds. GEOHASH re-encodes with the
standard [-90, 90] latitude range and returns eleven base32 characters, with
the compatibility final character zero. Ordinary ZSet scores also decode;
negative/nonfinite/out-of-range conversions are defined without invalid C++
casts. The saturating behavior matches the local ARM64 reference; other
reference platforms can differ for scores outside the unsigned 64-bit range.

GEODIST uses the spherical haversine formula with radius 6372797.560856 meters,
converts m/km/ft/mi units and returns a bulk string with four decimal places,
using nearest-even rounding. GEOPOS coordinates are bulk strings in RESP2 and
doubles in RESP3. Missing GEOPOS entries are RESP2 null arrays; missing GEOHASH
or GEODIST values are null bulk strings. RESP3 uses nulls for both. Empty
member lists for GEOPOS/GEOHASH return arrays after the usual key type check,
matching the reference metadata's minimum arity of two.

## Storage and execution

There is no separate GEO blob, JSON payload or new record format. Both ZSet
indexes and member chunks remain private pages capped at 16 KiB. A 40,000-byte
member is split across existing chunks. Updates retain logical key locking,
execution admission, one native batch commit and the existing observer/replay
protocol. Read commands use the existing paged ZSet transaction, including
legacy JSON import without logical WATCH invalidation.

Command metadata comes from the isolated local Redis reference using the
catalog generator. It supplies @geo categories, read/write key permissions,
COMMAND INFO/DOCS/key discovery, statistics, queue admission and replica
write rejection. The separate handler does not add branches to GET or other
ordinary String handlers. Cluster routing uses only the GEO key; members and
unit names are not keys.

## Scope

The core increment implements the four commands above. Indexed GEOSEARCH
and GEOSEARCHSTORE are now available; see [GEO search](GEO_SEARCH.md).
GEORADIUS/GEORADIUSBYMEMBER and their read-only variants are now available
through the same score-index engine; see [legacy radius commands](
GEO_RADIUS.md). Full Redis compatibility, initial sync,
PSYNC, Redis RDB/AOF, blocking commands, Pub/Sub and scripting remain active
work. Existing cross-physical-database EXEC restrictions are unchanged.

## Validation

`geo` checks known scores/hashes/distances, nearest-even text, 100,000 seeded
coordinate round trips, arbitrary score conversion, inclusive boundaries,
legacy ZSet import and a 40,000-byte member plus 240 entries. Native commit
probes assert every root and private page is at most 16 KiB on MDB and oneTBB.

`geo_commands` compares stable results and error precedence against the local
modified Redis 8.6.2 reference, including random coordinates, exact scores,
wire values, binary names/options, duplicate options, invalid later triples,
wrong types, TTL, DB selection, EXEC and command/key metadata. Independent
typed checks cover both protocols. Eight clients add 320 unique points and
compare the complete ordered score index. WATCH/ACL tests run against both
servers. RocksDB/WiredTiger configurations restart DB 3; private RPC replay
checks grouped EXEC changes. Cluster checks verify routing precedes coordinate/unit/syntax parsing, and
that redirection errors do not leave an extra response before the following
PING.

## GET regression measurements

The prior inline-String revision and final GEO revision use identical C++11
Release flags within each backend. Runs use two server threads, four clients,
a 64-byte hot value, 256-command pipelines, 4096 measured batches per worker,
16 warmup batches, disabled active expiration and three rotating rounds.
Each backend checks 50,331,648 responses. The backend runs execute sequentially
without build/test load.

| Backend / protocol | Prior PKV, requests/s | GEO revision, requests/s | Change |
| --- | ---: | ---: | ---: |
| MDB / RESP2 | 2,107,433 | 2,084,843 | -1.07% |
| MDB / RESP3 | 2,123,828 | 2,090,816 | -1.55% |
| oneTBB / RESP2 | 2,055,088 | 2,071,412 | +0.79% |
| oneTBB / RESP3 | 2,048,411 | 2,055,330 | +0.34% |

These measurements compare PKV revisions on macOS ARM64. Three samples per
variant do not establish statistical significance. They cover hot inline GET,
not GEO search, mixed workloads, paged values, disk performance or stock Redis.
See [MDB raw results](benchmarks/geo-get-mdb-2026-11-01.json) and
[oneTBB raw results](benchmarks/geo-get-tbb-2026-11-01.json).

## GEO-core revision validation

The GEO-core revision passed 902/902 full-matrix CTests on macOS ARM64:
MDB Debug/C++11 Release 103 each; oneTBB Debug/C++11 Release 112 each;
RocksDB Debug/Release and WiredTiger Debug/C++11 Release 118 each. Debug
configurations used ASan/UBSan. Existing timeouts, concurrency checks and disk
synchronization settings were retained. All 119 source fingerprints remained
fixed, and the measured Release binaries match the full-matrix binaries.
Linux/GCC was not validated.

Every configuration retained 560 stable GEO differential observations, 14
independent typed GEO observations, 284 typed RESP3 comparisons, 191 stable
HLL observations and the existing INFO, CONFIG, ACL, authentication, client,
transaction, bounded-data and replication suites. New C++ files are each
under 200 lines; the modified service constructor is 161 lines. The longest
new Python function is 96 lines. Modified C++ lines stay within 80 columns.
See [machine-readable validation evidence](
benchmarks/geo-validation-2026-11-01.json).
