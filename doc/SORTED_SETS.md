# Sorted Set commands and ranges

Existing commands now use `dao::paged_zset` on `mdb` and `mdb_tbb`, with native
member/score indexes, atomic legacy migration, TTL/COPY/RENAME/ZSCAN and PKV2
replication. JSON format/view descriptions below document legacy records and
reference fixtures. See [current storage](ZSET_STORAGE.md).

The tested RESP2 memory server now supports ZADD, ZINCRBY, ZREM, ZCARD,
ZSCORE, ZMSCORE, ZRANGE, ZREVRANGE, ZRANK, ZREVRANK, ZPOPMIN, ZPOPMAX, ZMPOP, ZRANDMEMBER and ZSCAN.
Range extensions add ZRANGESTORE, ZRANGEBYSCORE/ZREVRANGEBYSCORE,
ZRANGEBYLEX/ZREVRANGEBYLEX, ZCOUNT/ZLEXCOUNT and
ZREMRANGEBYRANK/ZREMRANGEBYSCORE/ZREMRANGEBYLEX.
This is a foundation, not a claim that every Redis Sorted Set command or option
is complete.

ZADD supports NX/XX, GT/LT, CH and INCR, including repeated flags, ordered
repeated members, incompatible-option errors and nil replies for rejected INCR
conditions. All input scores are parsed before mutation. The DAO performs one
locked update, so failed input/arithmetic/encoding/storage does not partially
apply a request. ZINCRBY rejects a NaN result such as inf plus -inf, but accepts
infinite scores and finite arithmetic that overflows to infinity, as Redis does.
Existing key TTLs are retained. New keys start persistent, missing XX requests
do not create keys, and removing/popping the final member deletes the key.

ZSCORE/ZMSCORE distinguish missing members from zero scores; ZMSCORE preserves
request order and duplicates. Rank commands support WITHSCORE (singular), with
RESP2 nil array replies for missing WITHSCORE results. Rank ranges support
inclusive indexes, negative indexes and WITHSCORES (plural). ZRANGE supports
BYSCORE/BYLEX, REV and LIMIT; ZRANGESTORE uses the same selection syntax but
rejects WITHSCORES. Score bounds accept inclusive numbers and exclusive `(number`,
including infinities. Lex bounds use inclusive `[member`, exclusive `(member`
and `-`/`+` infinity sentinels; member bytes may be empty or binary. Reverse score
and lex requests take the upper bound first. Negative LIMIT offsets select
nothing, zero count selects nothing, and negative counts select the remainder.
Redis's rank LIMIT count -1 exception is accepted and ignored; other rank LIMIT
counts are rejected. Same-score ordering is by binary member bytes and reverses
in reverse mode. Lex operations match Redis for equal-score collections; Redis
leaves results undefined when scores differ. pkv deterministically orders those
mixed-score lex queries by member bytes without claiming reference parity.

Range removals preserve TTL, avoid writes for empty selections and delete the
key when its last member is removed. ZRANGESTORE locks both keys in common stripe
order, validates/encodes before committing one destination change, overwrites
any destination type, clears its TTL, and deletes it for empty/missing/expired
source selections. Source and destination may be the same key. Source wrong
types, malformed records and failed encoding/storage preserve the destination.
The backend needs only one effective write/delete, so range stores also work on
legacy backends under the shared lock discipline; this does not establish disk
engine crash durability.

ZSCAN returns a cursor plus flat member/score pairs, with MATCH on member bytes
and COUNT as a hint. Tokens are portable across connections to the same running
node/database. It uses the existing 256 stable member buckets, preserves live
bytes/TTL and covers continuously present members during mutations; it does not
provide a whole-iteration snapshot. Its optional codec accessor has a
materializing fallback; the JSON override reads the owning member map without
copying it or building score order. Scores use the same formatter as ZSCORE.
See [scan semantics and costs](SCAN.md) for filtering, cursor validation and
per-page whole-record parsing limits.

ZPOPMIN/MAX return flat member/score arrays in
RESP2 and support nonnegative optional counts, including zero and counts larger
than the collection.

ZMPOP takes a positive key count, keys in priority order, MIN/MAX and optional
positive COUNT (default 1). It locks all distinct keys in the shared stripe order,
then checks them in request order using one expiry timestamp. Missing/expired
keys are skipped; the first live wrong type returns WRONGTYPE. Once a live Sorted
Set is selected, later values are not decoded or type-checked. Duplicate key
arguments do not cause repeated pops. Only that selected key changes, its TTL is
retained, and draining it deletes it. Count is capped at that collection's size;
it never continues into another key in the same request. The RESP2 reply is
`[key, [[member, score], ...]]`; no candidate returns a nil array.

The callback completes selection/encoding before one effective write/delete.
Lookup, corruption, encoding or commit errors clear the selected key and popped
output; tested legacy single-write backends are supported. All selected keys
still have raw backend lookups during transaction preflight. Skipped expired
records are logically absent but physical cleanup is deferred to avoid extra
writes. The command copies raw snapshots of all requested keys; the selected
record is materialized, sorted and rewritten. Native ordered/chunked storage and
bounded lock time remain future work. This does not establish disk crash
atomicity or replication failover behavior. All keys participate in cluster slot
validation, including later/duplicate candidates.

ZRANDMEMBER without count returns one member or a nil bulk string. With count it
returns an array; positive counts select unique members and clamp to population
size, zero returns an empty array, and negative counts sample with replacement.
WITHSCORES requires count and returns flat member/score pairs under RESP2. The
parser validates count and score-array overflow bounds before lookup, matching
the tested reference error precedence. Even a zero count checks the key's type.
Missing/expired keys return empty arrays for counted requests. Existing member
bytes, scores and TTL are retained, and each reply samples a single owning view
while other clients may update/replace the key.

The Set sampler is now a shared indexed collection helper: partial Fisher-Yates
selects a uniform subset without replacement, and independent uniform indexes
select with replacement. A one-element request walks to one random position;
other partial samples build a pointer index without copying all member strings.
Taking every unique member copies them directly. An optional `zset_sample`
accessor keeps custom codecs compatible through a materializing fallback; the
JSON override uses its owning member map and does not build score order. The
worker-local generator is noncryptographic. Unrepresentable output lengths are
rejected before allocation, with separate pkv-only extreme-request tests; this
is not a configurable response-memory or CPU budget. Full JSON decode and an
O(N) pointer index for partial samples still limit large-key performance.

ZUNION/ZINTER/ZDIFF and their STORE variants accept Set and Sorted Set sources;
Set members contribute score 1. Union/intersection support WEIGHTS and AGGREGATE
SUM/MIN/MAX, and read variants accept WITHSCORES. Repeated keys retain their
individual weights and contribute repeatedly. Repeated options use the last
WEIGHTS/AGGREGATE value; difference rejects those options and retains scores from
its first input. Output is ordered by score then binary member bytes.

ZINTERCARD returns only intersection size, optionally capped by nonnegative LIMIT
(0 means unlimited). It validates every source before returning for missing keys
or reaching LIMIT. It iterates the smallest source and probes membership through
owning views, stopping as soon as the limit is reached; no intersection result
map is constructed. SINTERCARD now also uses this visitor boundary to avoid
copying its smallest candidate Set. JSON overrides for `set_visit`, `zset_visit`
and `zset_lookup` avoid full-map copies; custom codecs retain materializing
fallbacks. Visitor false means successful early termination, while an accessor
false indicates failure and clears partial command output.

All source/destination keys share one locked transaction. All source types are
validated before parsing optional weights/aggregation/LIMIT, including when a
missing source makes an intersection empty. The DAO's option callback runs under
these locks and must only parse options, without I/O, retained references or DB
recursion. STORE overwrites any destination type, clears its TTL, deletes it for
an empty result and permits aliasing a source. Sources retain bytes/TTLs; expired
source cleanup is deferred. Raw lookup preflight includes all requested keys.
Only one destination changes, so legacy single-key commit primitives suffice;
encoding or commit failure preserves it and clears result/count. Disk crash and
replication commit guarantees remain separate work.

For union, each NaN product such as inf*0 is normalized to zero before aggregation.
For intersection, the seed product is normalized; subsequent NaN contributions
make SUM zero and leave MIN/MAX unchanged. Opposite infinities under SUM also
produce zero. Sources are sorted by cardinality with the platform qsort used by
the tested reference; replacing it with stable_sort changed nonassociative SUM
results for equal-size inputs. A paired 600-comparison cancellation regression
now checks this against the local reference, accepting only the disclosed signed
zero text exception. Floating source ordering across different C libraries and
platforms remains unverified; no cross-platform bitwise guarantee is claimed.
The operation still decodes entire source records and materializes the result
map for union/intersection/difference. LIMIT does not bound JSON decoding, and
large-key memory/lock duration still needs native/chunked storage and measurement.

Blocking commands (BZPOPMIN/BZPOPMAX/BZMPOP) remain unfinished. A command's presence in the registry does not imply
all Redis options are implemented. RESP3 remains outside the current target.

## Scores and record representation

Logical records add `value_kind::zset` and a member-to-double map. The codec
interface has optional full-value, size, batch score, rank, selection, count, scan and sampling accessors;
existing custom codecs remain source-compatible and explicitly reject unsupported
writes. Default accessors provide materializing fallbacks. DAOs contain no yyjson
types. JSON uses a new v1 type with string-encoded scores:

```json
{"V":1,"T":"Z","E":-1,"P":-1,"D":[["member","1.25"],["other","inf"]]}
```

The array must be nonempty, each entry must be exactly two strings, member bytes
must be unique, and score text must parse without NaN. Infinities are strings,
not nonstandard JSON numbers. Encoding enumerates by member name for deterministic
records; query ordering instead uses score then member bytes. Unknown future
versions, duplicate members and malformed score payloads are preserved and
reported as errors. Legacy v0 export is rejected. Previous builds cannot read
`T: "Z"`; downgrade requires a tested backup/export plan. Other type encodings
and the v1 millisecond deadline representation remain unchanged.

The JSON view owns its member map after the input/document is released. It builds
a vector of pointers into that immutable map only when rank/range access needs
score order. `call_once` publishes the index safely for concurrent reads of one
view, and copying the view is disabled so pointers cannot refer to another
view's map. Cardinality, score lookup and ordinary mutations avoid constructing
that order index. Score selections binary-search that index and copy only the selected window.
Lex selections use the member map directly, avoiding score sorting. Score counts
use index distances; lex counts traverse the matching map interval without
copying members. New optional accessor fallbacks preserve existing custom codecs.
Range reads copy only selected members after index construction.
Each command still parses the complete JSON record; mutations materialize/copy
members and rewrite that record, and pops currently sort a materialized set.
This does not provide Redis's large-set time complexity or performance parity.
Chunked/native ordered storage and measured alternatives are still needed.

Score parsing uses a C numeric locale independent of process locale, consumes all
input bytes, accepts nonzero subnormals and explicit infinities, and rejects NaN,
leading whitespace, trailing bytes/NUL, overflowed numeric input and underflow to
zero. Score-range endpoints use a separate parser matching the tested reference
rules: empty text means zero, a bare `(` means exclusive zero, overflow/underflow
may yield infinity/zero, and hexadecimal endpoints are rejected. Full-byte/NUL
validation is retained for score endpoints. Hexadecimal ZADD score inputs have
reference coverage. Formatting uses the
standalone fpconv dependency plus integer/zero/infinity handling. The vendored
files retain their Boost Software License 1.0 and copyright notices; see
[dependency origin](../third_party/fpconv/ORIGIN.md).

pkv preserves IEEE negative zero. The local Redis reference's compact listpack
encoding can canonicalize -0 to 0, while its noncompact encoding can preserve -0.
Tests explicitly accept either textual zero for the compact zero scenario and
verify a noncompact binary-member case retains -0. Exact reply-byte parity for
that encoding-dependent case is not established; the values compare as equal
Sorted Set scores. Other fixed canonical score cases are checked byte-for-byte,
and 10,000 random non-NaN doubles round-trip through formatting/parsing with
identical bits. This distinction must not be hidden by claiming universal
byte-for-byte Redis parity.

## Validation and remaining limits

The socket suite has 2,259 basic and 2,458 range Sorted Set assertions per server,
plus 908 sampling, 2,268 multi-pop and 1,821 algebra assertions, with 70 separately
counted ZSCAN scenarios. A further 600 live paired floating comparisons run when
the reference is configured. It includes binary
and empty members, ties, signed rank extremes, infinity/subnormals/hexadecimal
inputs, invalid-float preflight, conditional/increment/CH semantics, exact TTL
retention, TYPE/SCAN TYPE/expiry/PERSIST integration, wrong types and arity, a
200-step model, and concurrent mutation/pop scenarios. Eight clients obtain all
800 increment scores, retain 240 independently added members and pop 320 members
exactly once. Range tests enumerate 100 score and 81 lex intervals, both
directions, pagination extremes, option errors, TTLs, same-key stores, missing and
expired sources, removals and four-client store/read snapshot races. Cluster
routing totals 338 assertions per server, including CROSSSLOT and same-slot
ZRANGESTORE, ZMPOP and single-key ZRANDMEMBER routing; score/member/bound arguments are
not treated as keys. Sampling tests cover 5-member binary and 300-member
populations, count/option errors, TTLs, replacement and unique sample properties,
and five clients sampling while complete populations are atomically replaced.
ZMPOP adds a 260-step three-key model, wire-level nil-array checks, request
priority/duplicate-key/late-wrong-type behavior, binary members, TTL/deletion,
expired candidates, parser extremes and eight clients consuming 640 members
exactly once across two overlapping key lists. Storage fixtures inject failed
lookup/rewrite/delete/encode, preserve all bytes on failure, and require cleared
uncommitted output. Algebra coverage includes mixed integer/byte Sets and Sorted
Sets, weight/aggregate repeats, alias stores and TTLs, source-type error precedence,
NaN/infinity cases, a 160-step model and five clients reading/storing duplicate
source snapshots during increments. Codec fixtures refuse full collection
materialization while allowing visitor/lookup access; partial accessor failures
clear results, and callback termination is tested independently.
Ten pkv-only assertions reject unrepresentable replacement samples and verify
stored contents, TTL and connection usability, without sending allocation hazards
to the reference process. Shared sampler fixtures retain existing Set subset and
frequency checks and compare 1,000 fixed-seed Set/Sorted Set selections with
associated score preservation.

Codec/storage fixtures cover owning views, custom accessor fallbacks, unsupported
codecs/legacy export, corruption/future-version retention, failed reads/writes/
deletion, range store/removal/encoding failures, cleared failed results,
indexed-versus-custom accessor selection/count behavior under extreme pagination,
selective and fallback scan/sample accessors, failed accessor output cleanup,
scan byte/TTL preservation and concurrent scan/rank reads,
NaN arithmetic, unchanged conditional writes,
and the new type through generic expiry changes. A shared 250-member view is read
by four threads while its lazy index is first built. Available fr_FR.UTF-8 numeric
locale is tested without affecting parser/formatter behavior and then restored.
Both C++17 ASan/UBSan and C++11 Release are checked. This is bounded local memory
validation; disk engines, crash durability, replicated batch boundaries, broader
floating-input differential/fuzz coverage and production performance remain open.

## Native Set inputs

Set and ZSet commands use bounded native pages on `mdb` and `mdb_tbb`.
Mixed algebra reads native sources under one page transaction, preserving Set
score-1 semantics and weighted input order. Its native destination is written
atomically with source migration if required. Aliased overwrites release the
old native page family and clear the destination TTL. The old read-only page
bridge remains available to legacy DAO fixtures.
