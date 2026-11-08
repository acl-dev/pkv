# Logical records and storage codecs

Commands and DAOs operate on `value_record`, `record_header`, and the owning
`record_view` interface in `coder/record_codec.h`. They do not expose yyjson types.
The existing `json_string`, `json_hash`, and `json_key` names are retained for
source compatibility; their implementations now use the codec attached to the DB.

A DB owns an immutable shared codec. `db::open(path, watchers, codec)` can select
an implementation before workers start; the default is `json_record_codec(1)`.
Replacing a codec must not occur concurrently with requests. Encoding, decoding,
and ownership are codec responsibilities; expiry and atomic mutation are DAO/DB
responsibilities. A decoded view owns its data after the input byte buffer is
released. Its hash lookup can read one field without building an entire map.

## JSON formats

All string and field lengths are byte lengths. The JSON codec retains permissive
Unicode read/write flags so existing non-UTF-8 Redis byte strings remain readable.
These bytes are an internal format and need not be valid UTF-8 JSON for other
JSON implementations.

Legacy v0 records have no version marker:

```json
{"T":"S","E":-1,"D":"value"}
{"T":"H","E":1700000000,"D":{"field":"value"}}
```

`T` is `S` or `H`; `E` is an absolute Unix timestamp in seconds, or `-1` for no
expiry. The reader converts legacy seconds to logical milliseconds with checked
multiplication. Structurally invalid records and unsupported versions/types are
reported without deleting their bytes.

New writes use v1:

```json
{"V":1,"T":"S","E":1700000001,"P":1700000000123,"D":"value"}
```

`P` is the exact absolute Unix timestamp in milliseconds, or `-1`. `E` remains a
compatibility field: `ceil(P / 1000)` for expiring records, or `-1`. A v1 reader
uses `P` and checks that `E` is consistent. Duplicate metadata is rejected.
Unknown future versions are preserved, including records whose unfamiliar expiry
metadata might otherwise appear expired. Hash payload validation is lazy for
single-field reads and complete when materialized for mutation or HGETALL.

### Legacy JSON List payloads

Lists extend v1 with `T: "L"` and a nonempty array of byte strings:

```json
{"V":1,"T":"L","E":-1,"P":-1,"D":["left","","right"]}
```

Element order and duplicates are significant. A list record must contain only
strings; nulls, numbers, nested containers, wrong payload types, and empty arrays
are invalid. List decoding validates the entire payload before making a view
available. Commands delete a key when its last item is removed. Expiry changes
materialize the correct logical type and preserve all elements.

`record_view` has optional list materialization, size and range accessors. Their
defaults keep existing custom codecs source-compatible and explicitly fail when
lists are unsupported. JSON range reads copy only selected elements after full
JSON parsing. Because validated List arrays contain only strings, indexed reads
use yyjson's direct flat-array access. LPOS uses `list_positions` to compare
selected decoded elements without materializing a vector of strings. Its default
accessor scans a materialized list for codecs that only implement `list_values`;
codecs can override it to search their own representation. RANK, COUNT and MAXLEN
bounds are checked before scanning; negative ranks search from the tail and
returned indexes always refer to the head. Mutations currently materialize a vector and rewrite one JSON
record under the key lock; pushes/pops therefore have whole-record storage costs,
and no Redis-like constant-time list mutation or performance parity is claimed.
Chunked list storage and alternative codecs require further design and benchmarks.

List export through the legacy v0 writer is rejected. Builds preceding List
support cannot read `T: "L"`; do not downgrade a database containing lists without
a backup or a deliberate export/migration plan. This addition does not change the
encoding or interpretation of existing string and hash records.

### Set payloads

Sets extend v1 with `T: "U"` and a nonempty array of unique byte strings:

```json
{"V":1,"T":"U","E":-1,"P":-1,"D":["","one","two"]}
```

Encoding uses the logical set's ordered enumeration for deterministic bytes;
array order has no membership semantics. Decoding accepts any member order but
rejects duplicates, non-string members, wrong payload types and empty arrays.
Redis empty sets have no key. Legacy v0 readers/writers do not support this type;
the legacy exporter rejects it, and downgrading requires backup/export planning.
Existing string, hash and list encodings remain unchanged.

Optional `record_view::set_values`, `set_size` and `set_membership` accessors keep
existing codecs source-compatible. Default size/membership accessors use full
materialization; a codec may override them. The JSON Set view builds one owning
`std::set` index during decoding, releases the parser document and reuses the
index for cardinality and ordered batch membership queries. Returned membership
results preserve duplicate request arguments. Initial parsing/index construction
still costs at least linear work (ordered insertion is O(N log N)); mutations
copy/materialize members and rewrite the complete JSON record. Chunked storage,
compact/hash indexes and alternative encodings need benchmarks and migration
policies before adoption. This implementation does not establish Redis's Set
command time complexity or server performance parity.

### Sorted Set payloads

Sorted Sets use v1 `T: "Z"` with nonempty `[member, score-text]` pairs. Members
are unique byte strings; score text round-trips IEEE doubles, allows infinities
and rejects NaN. Using score strings avoids invalid JSON infinity literals.
Encoding is deterministic by member name; query order is score then member bytes.
Malformed/duplicate/future records are preserved. Legacy export is unsupported,
and older builds cannot read the type. Native expiry changes preserve the page family. Legacy expiry changes import
this logical kind into bounded pages while retaining member scores. Optional codec score/rank/range accessors
allow specialized owning indexes without exposing yyjson in DAOs. See
[Sorted Set storage and score details](SORTED_SETS.md) for zero representation,
lazy index ownership, performance limits and validation.

## Upgrade and downgrade behavior

The new reader accepts v0 and v1. There is no startup rewrite, bulk migration, or
modification of user data during these development tests. The codec can still emit v1 JSON for legacy engines. Native DAOs import old
records into their typed page layouts; legacy expiry/PERSIST changes now share
that importer and preserve the payload. The original deadline is retained
unless the command intentionally changes it.
Unchanged conditional operations leave the original bytes intact, except lazy
removal of a record already known to be expired.

Older readers may ignore `V` and `P` and see the rounded-up `E`. Their expiry can
lag the exact deadline by up to 999 ms. An older writer can discard v1 precision
metadata when it modifies a record. This is not a guarantee that arbitrary older
pkv builds can safely handle every v1 record or binary payload: test a backup
before a production downgrade. Rollback requires retaining a pre-upgrade backup
or an explicit offline export. There is no automatic downgrade migration tool.

`json_record_codec(0)` supports explicit legacy export for records with no expiry
or deadlines exactly divisible by 1000. It returns `unsupported_precision` for
other deadlines instead of silently rounding them. An operator must explicitly
choose how to handle precision loss before exporting those records.

## Extending codecs

Implement `record_codec::decode` and `encode`, plus an owning `record_view` with
payload accessors. Select the codec at DB open time. Read methods must be safe
for concurrent calls on the shared codec. Encoding errors must not commit bytes;
DAO mutation callbacks only request a DB write after successful encoding.
A new persistent encoding also needs legacy dispatch, version rejection, size
bounds, migration/rollback policy, corruption tests, and measured performance.

`tests/codec_test.cpp` runs the same DAOs against a separate non-JSON string codec,
checks legacy export, legacy-to-v1 mutation, binary payloads, exact deadline
round trips, malformed records, and preservation of unknown future formats.

## Initial codec measurement

A local macOS arm64 run used AppleClang 17, C++11 Release, yyjson 0.7.0,
100,000 iterations per case, 5,000 warmup iterations, and the median of five
samples with alternating format order. No concurrent build/test job was run
while collecting this sample. Hash values were 32 bytes each.

| Case | v0 / v1 bytes | v0 / v1 encode records/s | v0 / v1 decode-and-read records/s |
| --- | ---: | ---: | ---: |
| 32-byte string | 55 / 68 | 7.50M / 10.17M | 11.74M / 10.12M |
| 1-KiB string | 1047 / 1060 | 3.20M / 3.10M | 3.84M / 3.60M |
| 8-field hash | 382 / 395 | 3.94M / 3.70M | 5.40M / 5.02M |
| 64-field hash | 2956 / 2969 | 0.94M / 0.92M | 1.31M / 1.28M |

The fixed version/precision metadata adds 13 bytes in these no-expiry cases.
Measured rates vary with allocator, CPU state, payload, and platform; the small
string encode difference is not an asserted general speed improvement. Additional
metadata has a parsing cost, and the owning-view allocation is also included.
This is not a network benchmark or proof that pkv is faster than Redis. Keep the
reproducible `codec_bench` target for later comparison with alternative codecs and
measure complete command/backend paths before adopting a different default.


## Native List command storage

The JSON List layout above describes the legacy codec and compatibility
fixtures. The server now routes nonblocking List commands through a native
bounded-page DAO on `mdb`, with atomic first-access migration of old JSON Lists.
TYPE/TTL/DEL/overwrites and COPY/RENAME understand page ownership. See
`LIST_STORAGE.md` for layout, migration, replication and engine limitations.
Existing Hash commands now also use bounded Map roots/nodes/fragments, with
field-wise JSON migration, metadata-only TTL and independent COPY/RENAME. See
`MAP_STORAGE.md`. Set uses native membership pages; ZSet uses coordinated
member/score pages. See `SET_STORAGE.md` and `ZSET_STORAGE.md`.


## Native String command storage

Default String commands store PSR v2 roots with up to 1024 inline bytes,
or a 32-byte PSR v1 root plus deterministic owned 16-KiB chunks. TYPE, TTL, COPY,
RENAME, SCAN TYPE and PKV2 understand these roots. Old JSON values migrate
atomically on single-key access; MGET reads old/native values without migration.
Small v0/v1 compatibility events are bounded exports from inline roots. Full
String roots are never flattened into legacy frames. See `STRING_PAGES.md` for
layout, limits and rollback requirements.


PSR accepts BITFIELD final-field tails through 512 MiB + 8, while ordinary
String writes retain their 512-MiB bound. Binaries that enforce the previous
cap reject these extended roots. See BITFIELD.md for
checked offsets, downgrade implications and boundary fixture limitations.
