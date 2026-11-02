# Native String storage

`coder/string_page`, `db/string_store` and `dao/paged_string` now back all existing
server String commands on native mdb, oneTBB, RocksDB and WiredTiger. Generic
TYPE/TTL/SCAN TYPE, COPY/RENAME and PKV2 understand PSR ownership. Legacy String
records migrate under the key transaction on single-key access; MGET reads
legacy/native values without migration. Older binaries cannot read PSR roots;
rollback requires a tested backup or offline export rather than replacing the
binary against the new data. Engines without native page transactions reject
this path explicitly.

## PSR v1 layout

All integers use big-endian encoding. A root has a 32-byte header followed by
the inline bytes only when inline storage is selected.

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 4 | `PSR` and version byte 1 |
| 4 | 4 | Flags: 1 inline, 0 paged |
| 8 | 8 | Exact key expiry in ms; all-one bits mean no expiry |
| 16 | 8 | Logical String byte length |
| 24 | 8 | Data page count, zero for inline values |
| 32 | 0..1024 | Inline payload when length is at most 1024 |

Ordinary String writes are limited to 512 MiB. BITFIELD can extend its final
field up to eight bytes farther; PSR accepts 512 MiB + 8. Inline roots are at
most 1056 bytes;
paged roots are exactly 32 bytes. Data pages hold raw binary bytes, at most
16384 each, at deterministic private IDs 1 through the page count. All but
the last page are full. The private address includes the logical owner key,
so empty/binary owners and ordinary user keys cannot collide with page IDs.
Data pages need no extra encoding or per-value JSON escape expansion.

The page count is derived from the byte length and validated when opening the
root. Unknown versions/flags are unsupported; invalid sizes, TTL encodings,
inline lengths, counts and trailing bytes are rejected. Codec failures retain
the previous output. Empty String values have valid inline roots and remain
distinct from nonexistent keys.

## Transaction operations

A context belongs to one `page_transaction` and one owner. Caller policy decides
type checking, expiry cleanup and whether a nonnative record may migrate.
`initialize` explicitly resets the owner's family for a new/replaced value;
`assign` retains the context TTL and replaces bytes. `clear` removes the key.
The context must finish before commit; forgetting finish aborts a dirty context.
Ignored structural/size/read errors poison the transaction.

`range` uses String index normalization and reads only intersecting chunks.
`write` preserves old bytes in changed pages, fills growth gaps with zeroes
and stages only overwritten pages plus newly grown pages. APPEND uses the same
path. Missing chunks are corruption, not implicit zero-filled holes: growth
explicitly stores the zero pages. Boundary transitions copy the bounded inline
prefix into the first data page. Inline-to-paged and paged-to-inline changes
commit their root and page-family changes together.

APPEND of an empty value creates an empty String when missing; an empty
SETRANGE-style write does not create a missing key. Metadata-only TTL edits
touch no data pages. `clone_to` copies validated pages independently, preserves
TTL and never materializes the complete String. It supports the existing
ordered cross-DB page-copy transaction; same-context/owner clones are rejected.

No single stored value exceeds 16 KiB. Whole-value reads still own their required
response, and atomic large writes/clones still stage all affected pages plus
transaction metadata. A far-offset write can allocate many explicit zero pages;
sparse storage and asynchronous disk scheduling are separate future work.
Chunk sizes are checked on access; range reads do not validate unrelated pages.
There is no new end-to-end checksum or power-loss guarantee.

## Verification

`native_string_pages` covers binary inline/page boundaries, strong codec error
outputs, a fixed-seed 500-operation std::string model per memory engine,
empty/missing distinctions, inline self-alias transitions, four-thread append
block uniqueness, cross-DB clone independence and exact TTL. It checks
missing/future/corrupt data, ignored errors, forgotten finish, family reclamation
and allocation-failure rollback across two owners.

An 8-MiB String locality fixture permits no individual allocation larger than
32 KiB while reading a two-byte cross-page range, changing one byte, appending
one byte and updating TTL. It must read three pages and write two. A later TTL
edit reads/writes no data pages. `rdb_string_pages`/`wdb_string_pages` check three
native connection lifetimes with binary owners, empty Strings, cross-page edits,
append and retained millisecond expiry in isolated temporary directories.

## Command integration

SET/SETNX/GETSET/SETEX/PSETEX and SET options use native root/page transactions.
Conditional failures retain the old record; GET can return old bytes before
replacement, and KEEPTTL retains the exact deadline even when replacing another
type. An unconditional SET can intentionally replace an unreadable old record.
MSET/MSETNX stage every replacement in one native transaction with preflight
conditions and clear their applied flag on failed commit.

GET, ranges, STRLEN, APPEND, SETRANGE, integer/float increments and GETEX/GETDEL
use the typed store. A per-transaction timestamp governs expiry checks. Numeric
validation rejects oversized native values from their length before reading
data pages. String chunks and inline bytes are never encoded as JSON on the
default storage path. Legacy JSON migration reads bounded slices from the
existing owning view and preserves exact TTL; invalid numeric commands and
failed commits must not publish a partial migration.

MGET remains read-only, including for old JSON values and expired keys. It holds
the same ordered key locks, handles duplicates/ordering and wrong-type nils, and
does not turn a cross-physical-path read into unsupported distributed writes.
Cross-path disk mutations retain the existing explicit preflight rejection.

COPY/RENAME validate and independently clone the owned pages, including empty
Strings and replacement over other native types. TTL/PERSIST edit only bounded
roots. Small range/point operations need only their touched chunks; GET and
SET GET still allocate the required owning response.

PKV2 publishes native transaction boundaries and ROOT_REPLACE on family resets.
Old v0/v1 subscribers can receive only page-free inline String roots as bounded
JSON SET/DEL events. Conversion is charged against their actual encoded output
bytes, including JSON expansion. Unsupported pages/formats, conversion failure
or budget exhaustion close the affected peer. Old protocols keep per-root
events, not PKV2 multi-key atomicity. Large Strings require PKV2. The existing
64-MiB encoded page-transaction/publication caps still apply; a locally valid
large write can invalidate subscribers if it exceeds those caps. Initial sync,
replay and Redis replication parity remain unfinished.

Component-stage validation (2026-10-30): all eight build configurations compiled
the new component and tests. The selected native String, page-transaction and
record-codec fixtures passed 28 entries: three each on C++11/C++17 memory and
oneTBB builds, four each on RocksDB Debug/Release and WiredTiger C++17 sanitizer
and C++11 Release builds. Both disk engines retained bytes/TTL through three
open/close lifetimes. These are targeted component checks; the expanded full
32/33/35-test server matrices were not rerun in this component-only stage.


## Single-owner fast path

The native DAO opens PSR roots directly, avoiding a second inline decode and
an unused generic metadata view. A transaction with one distinct owner acquires
its existing key stripe directly. Multi-owner transactions retain the ordered,
deduplicated stripe locking used for atomic updates. Reserving the guard before
locking keeps allocation failure from leaving a locked mutex behind. These
changes retain synchronization with ordinary key reads and writes; oneTBB page
transactions are not fully lock-free.


See the [String DAO microbenchmark](STRING_STORAGE_BENCHMARK.md) for method,
raw samples, latency results and measurement limits.


## Default-path validation (2026-10-30)

After native command integration and the final single-key transaction fast path,
all eight configurations compiled and completed their full CTest suites:

| Configuration | Passed | Wall seconds |
| --- | --- | --- |
| C++17 memory ASan/UBSan | 33/33 | 21.78 |
| C++11 memory Release | 33/33 | 57.67 |
| C++17 oneTBB ASan/UBSan | 34/34 | 58.70 |
| C++11 oneTBB Release | 34/34 | 58.53 |
| C++17 RocksDB ASan/UBSan | 36/36 | 39.51 |
| C++17 RocksDB Release | 36/36 | 57.80 |
| C++17 WiredTiger ASan/UBSan | 36/36 | 112.25 |
| C++11 WiredTiger Release | 36/36 | 106.11 |

These are 278 passing CTest entries across the final complete runs, including
model/locality/allocation rollback, concurrent updates, socket commands, cluster
routing, replication and disk reopen/process-kill fixtures. The core sanitizer
builds use ASan and UBSan; external database libraries are not instrumented.

The first final memory sanitizer run, concurrent with four other matrices,
failed `page_live_stream` at its existing 8-second server startup deadline. No
storage assertion or sanitizer report appeared. A complete memory rerun at
reduced concurrency passed all 33, retaining the deadline and assertions. Earlier
WiredTiger verification exposed a startup connection reset; the readiness loop
now retries that transient connection error within the same deadline, retaining
process-liveness and PONG checks. The two final WiredTiger matrices ran
sequentially and both passed all 36, using their existing 180-second socket
fixture timeout. These results do not establish Redis completeness or
power-loss/replication interoperability beyond the documented fixtures.

Relevant source lines satisfy the 80-column limit, Python fixture syntax checks
pass, and the tracked diff passes the CRLF-aware whitespace check. No commits or
external publication are part of this validation.


BITOP uses an ordered chunk producer and a late complete-family seal so the
output may alias an input without reading newly staged bytes as old input.
The writer clears TTL, enforces chunk lengths and removes stale tail pages,
including earlier staged edits. Output pages remain transaction-staged until
commit; bounded chunk scratch space does not imply bounded total transaction
memory. See [BITOP](BITOP.md).


BITFIELD's final-field tail extends the accepted PSR length cap by eight bytes
without changing its byte layout. Older binaries enforcing the previous cap can
reject such roots. Ordinary write/bit-offset limits remain separate; see
[BITFIELD boundary behavior](BITFIELD.md).
