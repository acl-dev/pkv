# Cursor scans

KEYS is now available for a complete coherent traversal; see
[keyspace semantics and cost](KEYSPACE.md). Native root scan indexes are
implemented by mdb, oneTBB, RocksDB and WiredTiger. Descriptions of the
original mdb/JSON path below are historical implementation detail; current
collection page layouts are documented in the native storage guides.
SCAN and WATCH use shared native metadata inspection without allocating a
payload view. Ordinary SCAN pages retain their existing weak-snapshot rules.

SCAN now uses an opaque unsigned cursor supplied by the client. It no longer
keeps one mutable cursor on a connection. HSCAN, SSCAN and ZSCAN use the same reply
shape: `[next-cursor bulk string, array]`. Start at `0`, pass each returned cursor
to the next request, and finish when it returns `0`. An empty result array can
have a nonzero cursor, especially after MATCH/TYPE/expiry filtering. Cursors,
page order and page lengths are implementation-specific; do not transfer tokens
between engines, Redis and pkv, or database lifecycles. Tokens work across
connections to the same running node/database. SELECT changes that scope; do not
continue a token from another logical DB. COUNT is a work hint, not an upper
bound on returned elements.

## Commands and filtering

- SCAN supports repeated MATCH, COUNT and TYPE options; the last option wins.
  TYPE names are case-insensitive, and an unknown name matches nothing.
- HSCAN supports MATCH, COUNT and NOVALUES. Without NOVALUES, each field is
  followed by its value; COUNT counts fields, not the flattened reply length.
- SSCAN supports MATCH and COUNT.
- ZSCAN supports MATCH and COUNT, returning each member followed by its formatted
  score. COUNT counts members rather than flattened elements; MATCH applies to
  member bytes, not score strings. NOVALUES is valid only for HSCAN.

MATCH is binary-safe, including NUL and non-UTF-8 bytes. The matcher supports
`*`, `?`, bracket classes/ranges, `^` class inversion and backslash escapes. It
uses constant stack space and no recursive wildcard calls. Literal/class/range
behavior is checked against the local Redis reference, including malformed
classes and signed byte ranges. Wildcard/escape cases also have an independent
1,000-case dynamic-programming oracle and a deep-pattern regression. Long
adversarial patterns can still require substantial matching work; a configurable
CPU/reply budget is not yet implemented.

Cursor parsing validates the complete input byte string and checks unsigned
64-bit overflow. It accepts Redis's positive/zero aliases such as `00`, `+0`,
`-0` and leading whitespace before zero. The local Redis reference uses strlen
for this input and can truncate a NUL suffix; pkv intentionally rejects that
suffix with `ERR invalid cursor`. Eight independent checks verify the error and a
subsequent successful PING, separate from Redis comparison scenarios.

For HSCAN/SSCAN/ZSCAN, cursor validation precedes lookup. A missing/expired collection
returns an empty completed scan before options are parsed, and a wrong-type key
reports WRONGTYPE before option errors. SCAN parses options before backend work.
Errors produce replies and leave the connection usable. Unsupported backends
return `ERR scanning is not supported by this storage backend`.

## Backend and collection cursors

The `db::scan_keys` API is stateless. Memory, oneTBB, RocksDB and WiredTiger
implement it through their own root indexes. The original mdb layout follows. Each logical memory shard now contains 256 stable buckets, with
an unordered_map allocated only for a bucket that is used. Keys are owned once;
there is no second full key index. The original CRC32 shard selection is retained,
and the quotient selects a sub-bucket. SCAN copies complete buckets until it
reaches its hint, then releases the native shard lock before DAO lookup/expiry/type
filtering takes key locks. It retains no unordered_map iterator across requests
or yields, so rehashing cannot invalidate a saved iterator. The opaque cursor
encodes a shard and bucket; restart an unfinished iteration at zero after changing
binaries or database lifecycles. No JSON record or cluster slot format changes.

For ordinary distributions, a small hint now copies roughly COUNT plus one bucket
instead of an entire shard. COUNT is still not an absolute bound: collision-heavy
buckets, large keys, large COUNT values and logical decoding can cause substantial
work. There is no production hard latency guarantee. Each logical shard needs
256 pointer slots (about 2 KiB on this 64-bit build), plus maps for used buckets;
empty allocated maps may be retained for reuse. This exchanges some index/map
memory and more page requests for smaller per-request key batches. Adversarial collision behavior and strict work/memory budgets remain
unfinished; disk and oneTBB index layouts are described in their storage guides.
Legacy `db_cursor` hooks remain for source compatibility with older engines but
are no longer used by server SCAN.

Collections use 256 fixed FNV-1a buckets of field/member bytes. A cursor advances
past whole buckets until its hint is reached or the buckets are exhausted.
Member positions remain stable when surrounding members are added/removed,
unlike offset pagination. HSCAN/SSCAN/ZSCAN expose optional codec accessors with
materializing fallbacks for custom codecs. JSON HSCAN validates field types and
duplicates and copies selected names/values; NOVALUES avoids copying field
values. JSON SSCAN reuses its owning Set index. JSON ZSCAN uses its owning
member map without building score order, copying the full map or materializing
unselected score text. Custom codecs can implement only the new `zset_scan`
accessor; failures clear partial pairs and the cursor. The default accessor
materializes through `zset_values` for compatibility with existing extensions. Every call still parses the
whole JSON record and scans member/field metadata to choose buckets, so small
COUNT values do not provide constant-time collection scans. Chunked storage and
persistent scan indexes need a separate migration/performance design.

A scan does not take one global snapshot or lock every key for the entire
iteration. Continuously present unexpired keys/members are covered; concurrent
insertions/deletions may appear or be absent, and callers should tolerate
duplicates. There is no defined snapshot of TTL/type changes across pages.
Native key scans filter lazy expiry under each key lock; HSCAN/SSCAN/ZSCAN read owning
collection views without rewriting live data or TTL. Corrupt/unsupported records
produce errors without rewriting their bytes. SCAN is node-local in cluster
mode; HSCAN/SSCAN/ZSCAN use ordinary single-key slot routing.

## Verification

The ordinary socket suite runs 141 SCAN/HSCAN/SSCAN plus 70 ZSCAN scenarios per
server, separately from 31,936 ordinary command assertions. The number of frames/pages differs by
implementation, so cursor values and counts of page replies are not compared.
Coverage includes 180 database keys, binary MATCH patterns, NOVALUES, type
filters, expiry, malformed arguments, continued connections, portable cursors,
and 220 stable fields/members while transient data changes between pages.
ZSCAN coverage adds all-byte member matching, score formatting, 400 stable
members during surrounding insert/delete operations, independently interleaved
cursors, two score writers during cross-connection scans, option/error precedence,
TTL preservation and type/expiry changes. It compares complete traversal results,
not engine-specific cursor values, page counts or order. Storage and codec tests
also cover a custom accessor refusing full materialization, its fallback,
partial-output failure cleanup, corrupt/future records and four threads sharing
an owning view while rank order is first built.
Storage tests exercise unsupported/error backends, logical lookup errors,
corruption, expiry cleanup, unchanged collection bytes and 200 continuously
present keys during 1,000 concurrent native insert/delete operations. Cluster
routing now totals 338 assertions per server, including node-local SCAN and
slot-routed HSCAN/SSCAN/ZSCAN. These do not validate cluster topology/failover or scan
performance/durability. A further native fixture puts 4,096 keys in one logical
shard and requires COUNT 1 to produce multiple pages with fewer than 256 keys
per page for that fixture, without omissions or duplicates. Fault injection now
checks 51 batch-commit and 15 native scan allocation points in this local build;
failed scan pages clear partial keys/cursors and keep stored keys unchanged.

## Local page-latency sample (2026-10-30)

A local macOS arm64 sample used AppleClang 17, C++11 Release without sanitizers,
16 logical shards, two workers, 30,000 binary keys, 128-byte values and COUNT 10.
Five rounds alternated the order of before/after binaries, creating fresh local
servers and data each time. No build/test jobs ran concurrently. Values below
are medians of per-round metrics; page timing includes the Python client's
parsing and local socket overhead. Binaries were captured before the Sorted Set addition from this uncommitted worktree,
so this is exploratory evidence, not a versioned/public performance result.

| Metric | Whole-shard pages | Stable sub-bucket pages |
| --- | ---: | ---: |
| Page p50 latency | 1.517 ms | 33.75 us |
| Page p95 latency | 1.654 ms | 41.25 us |
| Largest returned page | 1,880 keys | 20 keys |
| Requests for a full iteration | 16 | 2,102 |
| Full iteration elapsed time | 24.39 ms | 73.02 ms |
| Pipelined SET replies/s | 1.000M | 1.024M |
| Pipelined GET replies/s | 0.769M | 0.756M |

This improves small-page responsiveness at the cost of more round trips and a
slower full traversal with the same small hint. Clients seeking bulk traversal
can use a larger COUNT, with the corresponding per-request work. The short
10,000-operation SET/GET controls are too limited/noisy to establish a throughput
advantage or absence of regressions across workloads. No comparison with Redis
was performed, and this does not establish overall server performance superiority.

Raw [rounds and summary](benchmarks/scan-buckets-2026-10-30.jsonl) and
[environment/binary hashes](benchmarks/scan-buckets-2026-10-30-meta.json) are saved.
Re-run against retained baseline/current Release binaries while other jobs are idle:

```sh
python3 tests/scan_bench.py --before /absolute/path/to/before-pkv \
  --after /absolute/path/to/after-pkv --keys 30000 --count 10 --rounds 5
```

The script verifies all keys were returned exactly once in each unmodified
iteration, includes pipelined SET/GET controls at depth 128, and cleans up its
temporary servers/data. It does not benchmark concurrent scan/write tail latency,
resident memory, disk engines or crash recovery.
