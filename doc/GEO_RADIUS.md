# Legacy GEO radius commands

GEORADIUS, GEORADIUSBYMEMBER and their `_RO` variants now use the same indexed
search and paged PZR ZSet transactions as GEOSEARCH. Together with the existing
commands, PKV's ten @geo command names match the local reference inventory.
No new value encoding or whole-ZSet JSON serialization is introduced.

## Query and store behavior

Coordinate queries accept longitude, latitude, radius and unit. Member queries
use the decoded coordinate of the requested member. Both support m/km/ft/mi,
ASC/DESC, COUNT, ANY and WITHDIST/WITHHASH/WITHCOORD. Candidate selection uses
at most nine score-index ranges; COUNT ANY stops early and sorted limited
queries retain bounded top-k results. Existing limits on individual roots and
private pages remain 16 KiB, including chunked 40,000-byte members.

The two write-classified commands additionally support STORE and STOREDIST.
When these options repeat, only the last target is changed and its last option
selects geohash scores or full-precision distance scores in the requested unit.
Intermediate targets remain untouched. Empty results delete the final target;
nonempty results replace its type and clear its old TTL. Source TTL is retained
unless the source is also the final target. Source reads and target changes
share one native transaction. Source/target aliasing, EXEC workspaces and
private RPC page batches use the existing implementation.

Source type checks precede argument parsing. A missing member in an existing
source errors before radius/unit validation. For a missing source,
GEORADIUSBYMEMBER skips the base radius/unit parse while still validating
optional parameters, matching the reference's legacy behavior. Coordinate
queries always validate their base coordinates/radius/unit. STORE/STOREDIST
are not accepted by `_RO` variants and cannot be combined with WITHDIST,
WITHHASH or WITHCOORD. ANY requires COUNT.

GEORADIUS and GEORADIUSBYMEMBER are classified as writes even without a store
option. Read-only replicas therefore reject them. Use their `_RO` variants or
GEOSEARCH for read-only requests. All ten GEO commands preserve the existing
RESP2/RESP3 coordinate, distance, integer and null reply rules.

## Dynamic keys and admission

The command catalog generator now preserves keyword-based begin searches.
The resolver supports forward and reverse keyword scans, absent optional
keywords and invalid dangling matches. COMMAND INFO retains reference facts.
COMMAND GETKEYS/GETKEYSANDFLAGS use the first STORE match and first STOREDIST
match, in key-spec order, as the reference does; these lists do not necessarily
identify the final target when options repeat.

For execution, the target is resolved from optional arguments, consuming each
STORE/STOREDIST value and retaining the last one. Numeric arguments, unit names
and members are not targets. ACL and cluster/queued-command admission check
the union of public key-spec matches and this effective target. A shared key
pointer cannot suppress the effective target's write permission requirement.

This intentionally tightens an ambiguity in the local reference: repeated
STORE options can leave a later effective destination out of its first-match
ACL/key-spec list. PKV also requires permission and slot compatibility for
that later target. Public metadata remains compatible; PKV can reject such
ambiguous requests that the reference would admit. This prevents an unchecked
destination write and hidden cross-slot target. Ordinary single-target
requests retain reference behavior. Reference cluster preprocessing can also
reject an earlier, ignored metadata target from a different slot; PKV preserves
that check instead of routing solely by the last target.

## Validation

`geo_radius_keys` checks public first-match metadata, effective target inclusion,
read-only variants, forward/reverse/dangling keyword searches, units versus
options and shared key-pointer authorization. `geo_radius_commands` compares
578 stable observations and eight typed observations against the local modified
Redis 8.6.2 reference, and verifies all ten @geo names. It covers random
coordinates, result order, ANY, distance/coordinate/hash fields, duplicate
stores, type/error precedence, missing sources, binary options, 40,000-byte
members, metadata, DB 3 EXEC, WATCH/TTL, ACL, cluster routing, restart and grouped
private RPC replay. PKV-specific checks verify the extra actual-target guards.

The existing replica-policy suite now checks 76 write roots and 123 stable
observations, including both `_RO` radius commands. The expanded protocol suite
checks 301 typed RESP3 observations. The prior 1,487-comparison exploratory
radius run also passed before the formal corpus was added.

See [core GEO storage](GEO.md), [indexed search](GEO_SEARCH.md) and
[replica client-write policy](REPLICA_READONLY.md). Redis RDB/AOF, full
replication/PSYNC, complete cluster behavior, blocking commands, Pub/Sub,
Streams and scripting remain active work; complete Redis compatibility is
not claimed by this increment.

## Full matrix and GET comparison

The radius revision passed 942/942 CTests on macOS ARM64: MDB Debug/C++11
Release 108 each; oneTBB Debug/C++11 Release 117 each; RocksDB Debug/Release
and WiredTiger Debug/C++11 Release 123 each. Debug configurations used
ASan/UBSan. Existing timeouts, concurrency checks and disk synchronization
settings were retained. Every configuration executed the 578 stable radius
comparisons, eight typed radius comparisons, 301 typed RESP3 comparisons,
123 replica-policy observations and the existing storage/transaction suites.

The report records 141 source fingerprints. After the matrix, one CMake
fixture-list line was wrapped to meet the 80-column limit. All eight
configurations were regenerated: only the expected diagnostic backtrace
line numbers changed. Test commands, environments, timeouts, compilation
flags and server binaries remained identical. No C++ or test logic changed.
The measured Release binaries match the full-matrix binaries. New and
modified functions remain below 200 lines. Linux/GCC was not validated.

Matched GET runs compare the prior search/replica-policy revision with this
radius revision, using identical C++11 Release flags. Each backend uses two
server threads, four clients, a 64-byte hot key, 256-command pipelines,
4096 measured batches per worker, 16 warmup batches and three rotating
rounds. Active expiration is disabled. The backend runs execute sequentially
without build/test load; each checks 50,331,648 responses.

| Backend / protocol | Prior, requests/s | New, requests/s | Change |
| --- | ---: | ---: | ---: |
| MDB / RESP2 | 2,074,404 | 2,052,542 | -1.05% |
| MDB / RESP3 | 2,076,237 | 2,065,160 | -0.53% |
| oneTBB / RESP2 | 2,039,645 | 2,057,294 | +0.87% |
| oneTBB / RESP3 | 2,066,377 | 2,059,126 | -0.35% |

These are hot inline GET measurements of PKV revisions. Three samples per
variant do not establish statistical significance. They do not measure GEO
latency, mixed workloads, paged reads, disk performance or stock Redis.
The differential reference is a local modified Redis 8.6.2 binary. Historical
geometry and grid evidence is explicitly attributed to the preceding search
increment, whose geometry sources are unchanged. See [MDB raw results](
benchmarks/georadius-get-mdb-2026-11-01.json), [oneTBB raw results](
benchmarks/georadius-get-tbb-2026-11-01.json) and [validation evidence](
benchmarks/georadius-validation-2026-11-01.json).
