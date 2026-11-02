# Native BITFIELD and BITFIELD_RO

BITFIELD and BITFIELD_RO use native PSR String pages. GET supports i1..i64 and
u1..u63; SET and INCRBY support WRAP, SAT and FAIL policies. Offsets are MSB-first
and may use the #index form, scaled by the field width. Strict integer/type
parsing and all later subcommand errors are resolved before accessing a key.
Cluster routing precedes argument validation, as for the other String commands.

A command holds one key lock for all its suboperations and commits once. Later
GETs observe earlier successful changes. SET replies with the old value; INCRBY
replies with the new value. A FAIL overflow returns a null element and skips only
that suboperation. Other suboperations still execute. Existing key TTL is
retained. The write path first zero-grows to the farthest writable field, even
when every write later fails its overflow policy. That growth is committed and
replicated; it is not an absent key or a rolled-back command.

GET-only commands, including empty/OVERFLOW-only command forms, use a read-only
page transaction. They check existing value type, return zero for missing bits
and never migrate legacy JSON roots or create a missing key. BITFIELD_RO rejects
any parsed SET/INCRBY before key lookup, after validating the complete command.
The shared Redis parser also accepts policy-only OVERFLOW clauses in read-only
forms; pkv matches the local reference behavior without publishing a mutation.

The official references are [BITFIELD](https://redis.io/docs/latest/commands/bitfield/)
and [BITFIELD_RO](https://redis.io/docs/latest/commands/bitfield_ro/).
Exact corner/error behavior was also compared with local Redis 8.6.2 source and
its running server, which reports modified=1; this is not independent
stock-release certification.

## Arithmetic and boundaries

Arithmetic uses unsigned 64-bit accumulation/masks and explicit signed decoding,
without signed overflow, left-shifting a signed sign bit or converting an
out-of-range unsigned value to signed. Signed/unsigned SET and increment cases
are distinct. In particular, unsigned negative SET arguments convert to uint64
before overflow checking, so SAT clamps them high. Signed extreme-negative SET
also preserves Redis's wrapped max-delta check priority: for example SET i5 of
LLONG_MIN under SAT clamps high. The regression verifies that observed behavior
rather than replacing it with mathematical saturation. Ordinary signed INCRBY
bounds are checked before any signed addition could overflow.

A starting bit offset is below 2^32. A final 64-bit field may extend at most eight
bytes beyond the ordinary 512-MiB String write boundary. PSR metadata therefore
accepts lengths through 512 MiB + 8. Field growth/patch writes use that storage
limit. Ordinary SET/APPEND and SETRANGE end-offset checks retain their existing
512-MiB limits; SETRANGE can modify a valid prefix of an already extended value
without truncating it. GETBIT/SETBIT starting offsets remain below 2^32.
COPY/RENAME and BITOP can carry valid extended tails through existing page APIs.

#index scaling is checked before multiplication. Invalid negative/out-of-range
indexes cannot wrap into an accepted offset. The local Redis C helper multiplies
signed values before checking; inputs that overflow that multiplication have no
portable reference behavior. pkv deliberately rejects them. A separate pkv-only
fixture checks this, outside ordinary reference parity counts.

The PSR byte layout/version is unchanged, but older binaries that enforce the
previous 512-MiB metadata cap can reject these extended-tail roots. Downgrade
requires a tested backup/export. Production growth still creates every zero
page; no sparse String format is introduced.

## Storage, replication and costs

A field reads at most nine bytes from at most two chunks. Writes preserve partial
edge bytes and stage only touched pages plus the root. A pre-growth operation
can also stage many new zero pages. Chunk scratch space is bounded, while all
changed pages and reply/operation metadata remain transaction-staged until
commit. Long distant growth can consume substantial memory and hold its key
lock; cooperative scheduling and sparse storage are unfinished.

Native field operations never serialize the complete String. Writable legacy
values migrate under the key transaction and retain TTL. Extended legacy tails
use the ordered chunk producer instead of the ordinary append limit. Failed
migration/growth/field I/O cannot publish earlier successful suboperations or a
partial reply array. Metadata-only read paths leave legacy roots byte-identical.

PKV2 carries final root/page changes or growth, not an operation to re-execute.
Old v0/v1 peers retain only the supported inline String conversion. Large-growth
publication and queue caps still apply; initial sync/replay and Redis replication
interoperability remain incomplete.

## Tests

bitfield_storage checks 400 operations per memory engine against an independent
small-width byte model, legacy read-only roots, exact TTL, failed write pre-growth,
checked indexes and native/legacy allocation-failure rollback. Private pages are
probed before a legacy read could migrate/clear them. Earlier staged field writes
must roll back on a later missing page. Six threads increment a shared unaligned
i64 across a page boundary and must return every result exactly once.

An 8-MiB fixture limits individual C++ allocations to 32 KiB and checks six native
page reads/two writes for a compound cross-page SET/GET plus a separate GET-only
read. The maximum-start fixture intentionally constructs only the required tail
chunks under a synthetic large root, leaving unrelated prefix pages absent. It
checks local field growth, the 512-MiB-plus-eight length, preserved TTL and
ordinary command bounds without allocating a complete 512-MiB value. It does not
prove a full maximum-size value or its complete GET. Production zero-growth
loops are separately exercised with complete smaller values.

Disk reopen fixtures use three connection lifetimes to retain normal fields,
wrapped increments, failed increments, exact TTL and the synthetic extended-tail
neighborhood. The command fixture uses an arbitrary-precision integer model,
all signed/unsigned widths, extrema, null replies, #offsets, sequential policies,
strict errors, pipelines and eight concurrent socket clients. Generated streams
also compare replies and final bytes with the local Redis reference. Cluster
and real PKV2 replication fixtures cover field writes and all-FAIL growth.

Transaction/WATCH, ACL, blocking commands, streams, scripting, replication
initial sync and other Redis compatibility areas remain unfinished. These
fixtures are not completeness or performance certification.


## Validation (2026-10-30)

After the final production and fixture changes, all eight builds compiled and
completed their full CTest matrices:

| Configuration | Passed | Wall seconds |
| --- | --- | --- |
| C++17 memory ASan/UBSan | 39/39 | 51.30 |
| C++11 memory Release | 39/39 | 48.29 |
| C++17 oneTBB ASan/UBSan | 40/40 | 68.66 |
| C++11 oneTBB Release | 40/40 | 49.68 |
| C++17 RocksDB ASan/UBSan | 45/45 | 68.89 |
| C++17 RocksDB Release | 45/45 | 65.41 |
| C++17 WiredTiger ASan/UBSan | 45/45 | 122.20 |
| C++11 WiredTiger Release | 45/45 | 119.71 |

The final matrices contain 338 passing CTest entries, without a failed fixture
or retry. Every configured server and local Redis reference passed 1,276 common
BITFIELD checks. Each build also passed 200 generated reply/final-byte
comparisons and four separate pkv-only checked-index cases. Core sanitizer
builds use ASan and UBSan; external database libraries are not instrumented.
WiredTiger matrices ran sequentially and retained the existing 180-second socket
fixture limit. Both disk engines passed the three-lifetime ordinary-field and
synthetic-tail reopen checks. Existing container, String, bitmap, replication,
cluster and recovery fixtures also passed.

The initial directed reference run exposed signed extreme-negative SET SAT
priority: pkv and the independent mathematical model clamped low while the local
Redis helper clamped high. The implementation now matches that observed helper
ordering with defined unsigned arithmetic. The explicit regression and model
retain this distinction. The native storage maximum was also extended by eight
bytes to support valid final fields, while ordinary write and starting-offset
limits stayed separate. The final complete matrices include these changes.

New source lines satisfy the 80-column constraint and new functions remain
below 200 lines. Python syntax and CRLF-aware tracked diff whitespace checks
pass. No commit, deployment or external publication was performed. Full Redis
compatibility and performance superiority remain unproven; this is a feature
and regression checkpoint within the ongoing project goal.
