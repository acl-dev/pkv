# Indexed GEO search

GEOSEARCH and GEOSEARCHSTORE support FROMMEMBER/FROMLONLAT, BYRADIUS/BYBOX,
m/km/ft/mi, ASC/DESC, COUNT, ANY and their respective output/store options.
Searches retain the existing PZR ZSet encoding and 16 KiB private-page limit.
GEOSEARCHSTORE replaces the destination and clears its previous TTL; zero
results delete it. Source TTL is preserved unless source and destination are
the same key. STOREDIST saves full-precision distances in the requested unit.

## Indexed execution

A query plans at most nine geohash score ranges: center, north, south, east,
west, then diagonals. Adjacent-cell wrapping, precision reduction, exclusion
of unnecessary cells and range order follow the reference geohash helper.
The adapted planner's BSD copyright/license is retained in
[third_party/redis-geohash/LICENSE](../third_party/redis-geohash/LICENSE).

Each range uses indexed rank bounds and a streaming score cursor. Only
candidate points are decoded and checked against the circle or rectangle.
No full ZSet vector is built for a local query. COUNT ANY stops after enough
matching candidates, then applies a requested sort to that retained subset.
COUNT without an explicit sort defaults to ASC. Sorted limited searches keep
a top-k heap rather than retaining all matches. Equal-distance tie order is
not specified; distance ordering and count remain enforced. Unlimited replies
necessarily retain the requested output strings. One candidate member and a
bounded traversal path can still be materialized, including chunked members.

The source and optional destination are locked in one page transaction. All
source results are captured before destination replacement, including when
both names are equal. Invalid options, read failures, allocation failures and
failed commits do not publish destination changes. Native and existing legacy
ZSet imports share this path. EXEC workspaces and private RPC page batches use
the existing transaction machinery.

## Parsing and replies

Source type validation precedes option parsing. Missing source keys still
validate options; FROMMEMBER skips member lookup when the source is absent.
Repeated compatible origin/shape options retain their last value. ANY needs
COUNT. WITHDIST/WITHHASH/WITHCOORD cannot be combined with GEOSEARCHSTORE.
Cluster source/destination routing precedes data and option validation; members
and numeric arguments are not keys.

WITHDIST uses four-decimal bulk strings. WITHHASH returns an integer score.
WITHCOORD uses bulk coordinate strings in RESP2 and doubles in RESP3. The
integer score and coordinate reply formats match the existing GEO commands.

A high-precision regression found that a multiply/subtract expression could
be fused by the compiler, producing a tiny nonzero self distance. Keeping the
radian products as separate intermediate values restores exact zero, as in
the reference. The same correction protects zero-height rectangle filtering.
The previous frozen geometry source was reconstructed and its SHA verified:
1,699,977 of 1,700,000 self distances were nonzero before the correction; zero
were nonzero afterward. Four-decimal replies had masked these small residuals;
STOREDIST exposed them because it retains full precision.

## Validation

`geo_search` checks 100,000 self-distance/zero-box/grid cases, indexed reads on
4096 entries, COUNT ANY early stopping, a COUNT 1 query under a 16 KiB
single-allocation cap, chunked 40,000-byte result members, native read/commit
failures and 512 allocation budgets per memory backend for atomic stores.
The seeded local query uses 437 page lookups and COUNT ANY uses 193 on both
memory backends; these are lookup calls, not distinct pages or timings.

`geo_search_commands` compares deterministic reference results for circles,
boxes, random coordinates, unsorted order, COUNT ANY, nearest/farthest limits,
full-precision stored scores, dateline/pole cases, zero/infinite dimensions,
binary options, large members, type/error precedence, metadata and DB 3 EXEC.
Typed tests cover RESP2/RESP3. WATCH, ACL source-read/destination-write modes,
source/target aliasing, TTL, restart, grouped private RPC and cluster errors
are checked. [Replica write admission](REPLICA_READONLY.md) also applies to
GEOSEARCHSTORE while preserving GEOSEARCH reads.

An additional 1,250 random search comparisons and 250 stored result sets were
checked before the full suite. The optional `tools/check_geo_grid.cpp` compares
100,000 seeded grid plans with separately compiled BSD reference helpers.
Compile those helper objects from the reference checkout, then compile the
probe with this repository and that checkout's `src/` on the include path,
linking `common/geo_cells.cpp`, `common/redis_geo.cpp` and the two objects.
This adds no runtime dependency to PKV.

Legacy GEORADIUS/GEORADIUSBYMEMBER and their read-only variants now share this
engine; see [radius commands and dynamic key admission](GEO_RADIUS.md).
Redis RDB/AOF, initial sync/PSYNC, complete cluster behavior, blocking commands,
Pub/Sub and scripting remain active work. Existing cross-physical-database
EXEC restrictions are unchanged. The reference is the local modified Redis
8.6.2 binary; no stock Redis performance claim is made.

## GET regression measurements

Matched C++11 Release runs compare the prior GEO-core revision with the search
and replica-admission revision. Each backend uses identical compile flags,
two server threads, four clients, a 64-byte hot key, 256-command pipelines,
4096 measured batches per worker, 16 warmup batches and three rotating rounds.
Active expiration is disabled. Backend runs execute sequentially without
build/test load; each checks 50,331,648 responses.

| Backend / protocol | Prior, requests/s | New, requests/s | Change |
| --- | ---: | ---: | ---: |
| MDB / RESP2 | 2,132,136 | 2,096,032 | -1.69% |
| MDB / RESP3 | 2,066,413 | 2,070,569 | +0.20% |
| oneTBB / RESP2 | 2,062,374 | 2,057,824 | -0.22% |
| oneTBB / RESP3 | 2,067,060 | 2,061,766 | -0.26% |

These are macOS ARM64 hot inline GET measurements of PKV revisions. Three
samples per variant do not establish statistical significance. They do not
measure GEO search latency, mixed workloads, paged values, disk performance
or stock Redis. See [MDB raw results](
benchmarks/geosearch-get-mdb-2026-11-01.json) and [oneTBB raw results](
benchmarks/geosearch-get-tbb-2026-11-01.json).

## Search revision validation

The search and replica-policy revision passed 926/926 full-matrix CTests on macOS ARM64:
MDB Debug/C++11 Release 106 each; oneTBB Debug/C++11 Release 115 each;
RocksDB Debug/Release and WiredTiger Debug/C++11 Release 121 each. Debug
configurations used ASan/UBSan. Existing timeouts, concurrency checks and disk
synchronization settings were retained. All 137 source fingerprints remained
fixed throughout this final matrix. Measured Release binaries match the
full-matrix binaries. Linux/GCC was not validated.

Every configuration retained 434 stable GEO-search comparisons, eight typed
GEO-search comparisons, 119 replica-policy observations over all 74 write roots,
292 typed RESP3 comparisons, 560 core GEO observations and 191 HLL observations,
plus the existing configuration, INFO, ACL, authentication, client, transaction,
bounded-data and replication suites. Each memory backend's native store fault
sweep covers all 512 budgets with both failed and successful operations.

The legacy replica fixtures explicitly enable writes only for local setup and
outgoing mutation generation, restoring read-only mode for receiver assertions.
Their original atomic visibility, pagination, expiry and negotiation checks
remain. New and modified functions stay below 200 lines; modified C++ lines
stay within 80 columns. See [machine-readable validation evidence](
benchmarks/geosearch-validation-2026-11-01.json). The later radius increment
and its expanded matrix are documented in [GEO radius](GEO_RADIUS.md).
