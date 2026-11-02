# Native String BITOP

BITOP implements AND, OR, XOR, NOT, DIFF, DIFF1, ANDOR and ONE on the default
paged String DAO. Operator names are case-insensitive. NOT requires exactly
one source; DIFF, DIFF1 and ANDOR require at least two. Other operators accept
one or more, retaining input multiplicity. Invalid operator/count errors precede
source type checking. Cluster routing uses destination and all source keys,
starting at argument 2 rather than interpreting the operation as a key.

The result length is the longest input length, including trailing zero bytes.
Missing and expired inputs supply zeros; shorter inputs are padded on the
right. All source types are checked before computing, even when an AND input
is empty. A nonempty result replaces any old destination type and clears TTL;
all zero-length inputs delete the destination. A nonempty all-zero result
remains a String. Destination/source aliases read the original input bytes.
The [Redis BITOP reference](https://redis.io/docs/latest/commands/bitop/)
defines these operations; exact error/padding behavior was also checked against
the local Redis 8.6.2 source and running server, which reports `modified=1`.
This is not independent stock-release certification.

## Transaction and streaming layout

One ordered, deduplicated key-lock set covers the destination and all sources.
An argument-order input vector retains duplicates while a distinct-key cache
holds each source view once. Every source uses one transaction timestamp for
expiry. Native sources open bounded PSR metadata; legacy JSON inputs retain
owning views and read bounded slices. Read-only legacy inputs do not migrate.
Their initial decode still costs memory/work proportional to their old JSON
record; the native path does not add a complete decoded String copy.

For each output chunk, every source contributes its current chunk before the
output page is staged. The fixed String page geometry makes the same sequence
safe when the destination occurs anywhere in the input vector, including
multiple times. Earlier output chunks are never reread as old input. The
implementation folds inputs into an output buffer plus one auxiliary state
buffer and one reusable input buffer; it does not cache one chunk per input.
Word loads use memcpy for alignment safety, with a byte tail. ONE tracks bits
already seen in multiple arguments; repeated inputs are not deduplicated.

`string_store::replace_chunks` requests chunks in increasing page order. It
leaves the old family readable until all new chunks have been staged, then
`page_transaction::replace_pages` seals staged writes 1..page_count. The seal
rejects holes/deletes, drops earlier staged edits beyond the new length, and
marks the family for replacement at commit. A normal `finish` writes the new
root. An ignored callback/size error or forgotten finish aborts the transaction.
The inline path consumes its complete bounded input before resetting the old
family. The generic zero-length writer creates an empty String; BITOP handles
its empty-result deletion separately.

Each stored root/page is bounded by 16 KiB and chunk scratch space is bounded.
Atomicity still requires staging all output pages and transaction metadata, so
large operations consume memory proportional to their output. Source metadata
also scales with distinct/input key count. This is not a sparse format or a
cooperatively scheduled long-operation implementation. No PSR format change is
required. The existing String size and page/publication budgets still apply.

Only destination mutations commit. RocksDB/WiredTiger inputs can therefore
come from multiple configured physical paths while the output commits in one
physical database. This uses pkv key locks within the current process; it is
not a distributed transaction across independent writers or database instances.

## Replication

PKV2 publishes complete replacement roots/pages or destination deletion. A
subscriber receives the result, not an operation to re-execute against its own
inputs. Inline results retain the existing v0/v1 conversion, whereas paged
results require PKV2. Publication/queue caps can invalidate subscribers for
large results; initial sync/replay and Redis replication interoperability remain
unfinished. Ordinary generic metadata, COPY/RENAME and SCAN use the existing PSR
ownership rules.

## Tests

`bitop_storage` uses an independent per-bit count model over 200 operations per
memory engine, including duplicate/missing/binary owners and aliases. It checks
legacy read-only roots, retained source TTL/cleared destination TTL, overwriting
a Hash, expired wrong types and validation before short circuits. Allocation
sweeps cover native and legacy self-alias output; failed operations retain old
roots/pages and leave no extra output page. Missing-page failures after earlier
chunks were staged cannot publish a partial result. Unknown input versions
fail, while a non-input opaque destination can be replaced.

Replacement contract cases check incomplete seals, ignored producer errors,
forgotten finish and removal of both old and earlier staged tail pages. Six
threads toggle the same paged target via self-alias XOR; no update may be lost.
An 8-MiB self-alias output permits no individual C++ allocation above 64 KiB and must
read exactly 1024 input pages and write exactly 512 result pages. Full BITCOUNT
then verifies all output bits. The limit allows bounded transaction arrays;
a full input/result buffer would fail it. This is locality evidence, not a
throughput or peak-memory benchmark.

`bitop_commands` covers all eight operators, strict errors, wrong-type
precedence, longest-input zero tails, empty deletion, TTL, binary keys,
inline/page boundaries, source aliases, duplicate inputs and pipelines. Socket
races check alias updates and coherent two-key input snapshots while MSET
changes both inputs atomically. The optional local Redis server runs the common
cases and generated result/GET/TTL comparisons.

Disk reopen fixtures use three connection lifetimes for alias results, retained
bytes and cleared TTL. They also exercise read-only legacy/native inputs from
two physical paths and one-path output, including an alias into the legacy
source. Cluster tests check routing before operator errors; the real PKV2
source/receiver fixture checks paged replacements, aliasing, duplicate ONE and
empty deletion. Core sanitizer builds use ASan/UBSan; external database
libraries themselves are not instrumented.

[BITFIELD/BITFIELD_RO](BITFIELD.md) now use native fields; other compatibility
gaps remain. This feature
does not establish complete Redis compatibility or a performance advantage.


## Validation (2026-10-30)

All eight configurations compiled and completed their full CTest matrices after
the final production changes:

| Configuration | Passed | Wall seconds |
| --- | --- | --- |
| C++17 memory ASan/UBSan | 37/37 | 60.77 |
| C++11 memory Release | 37/37 | 57.75 |
| C++17 oneTBB ASan/UBSan | 38/38 | 53.65 |
| C++11 oneTBB Release | 38/38 | 57.37 |
| C++17 RocksDB ASan/UBSan | 42/42 | 62.34 |
| C++17 RocksDB Release | 42/42 | 57.70 |
| C++17 WiredTiger ASan/UBSan | 42/42 | 119.76 |
| C++11 WiredTiger Release | 42/42 | 114.87 |

These full runs contain 318 passing CTest entries. Every configured server and
the local Redis reference passed 1,198 common BITOP checks; each build also
passed 384 generated result/GET/TTL comparisons. The reference reports
`modified=1`. Both disk backends passed alias reopen and cross-physical-path
input checks. Existing protocol, container, transaction, recovery, cluster and
replication regressions also passed. WiredTiger matrices ran sequentially and
kept their existing 180-second socket compatibility limit.

The first unit precheck corrected a test expectation: XOR-cancelled inputs still
produce the longest input length, with zero tails. Production behavior did not
change for that correction. After the first full wave, fault tests were
strengthened to probe private pages before a legacy read could migrate/clear
them. The affected three configurations reran `bitop_storage` successfully.
The replica case was then strengthened to seed an existing paged destination
before empty-result deletion; the six configurations already completed reran
`replication_stream` successfully. The subsequent WiredTiger full runs included
both strengthened cases. These are nine supplemental passing CTest entries,
not retries of failed full-matrix fixtures. No full-matrix fixture failed.

New source lines stay within 80 columns and new functions within 200 lines.
Python syntax and CRLF-aware tracked diff whitespace checks pass. This verifies
the stated fixtures, not full Redis parity, peak-memory limits or an end-to-end
performance advantage. No commit or external publication was performed.
