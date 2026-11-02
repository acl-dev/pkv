# Native String bitmap commands

GETBIT, SETBIT, BITCOUNT and BITPOS use the default native String DAO on mdb,
oneTBB, RocksDB and WiredTiger. They operate on the existing PSR String format;
a bitmap remains a String and works with ordinary String commands, TYPE, key
TTL, COPY/RENAME and PKV2. No new whole-value JSON format is introduced.

## Semantics

GETBIT/SETBIT use MSB-first bit numbering, offsets 0 through 2^32-1 and strict
integer parsing. SETBIT returns the previous bit, preserves an existing key's
exact millisecond TTL, and zero-fills growth. Writing zero beyond the current
length still grows/creates the String. Reading beyond its length returns zero
without creating a key. Invalid offset/value arguments precede key type lookup.

BITCOUNT supports the whole value or inclusive start/end ranges with BYTE or
BIT units. BITPOS supports its optional start, end and BYTE/BIT unit, reporting
the absolute bit offset. Negative indexes count from the end of the selected
unit. Range normalization preserves the different reversed-negative-range
rules of BITCOUNT and BITPOS. Argument validation precedes type checking;
wrong-type values still fail when an otherwise valid range is empty.

BITPOS treats a missing key separately from an existing empty String. Searching
for zero in a missing key returns zero even for a supplied range; an empty
String returns -1. Without an explicit end, a nonempty scanned range consisting
only of ones can find the first virtual zero at the String's bit length. With
an explicit end, there is no virtual padding. An empty/out-of-bounds range of
an existing key returns -1. BITPOS stops after its first matching chunk.

The reference semantics are documented by Redis for
[GETBIT](https://redis.io/docs/latest/commands/getbit/),
[SETBIT](https://redis.io/docs/latest/commands/setbit/),
[BITCOUNT](https://redis.io/docs/latest/commands/bitcount/) and
[BITPOS](https://redis.io/docs/latest/commands/bitpos/). Edge-case behavior and
error precedence were also checked against the local Redis 8.6.2 source and
running reference, which reports `modified=1`; that reference is not an
independent stock-release verification.

## Storage and synchronization

Each command holds the existing key transaction lock. SETBIT on an existing
paged byte reads one 16-KiB page and stages at most that page plus its bounded
root; changing one bit cannot overwrite another concurrent bit update. Setting
an already equal bit within the existing length stages nothing. The old bit
reply is cleared on failed commit. Legacy JSON records migrate under the same
transaction and preserve TTL; failure cannot publish a partial migration.

GETBIT reads at most one native page; an out-of-range read needs no data page.
BITCOUNT/BITPOS scan only intersecting chunks and mask partial boundary bytes.
Population count uses unaligned-safe word loads and compiler popcount where
available, with a portable fallback. BITPOS skips words that cannot match and
checks the remaining bits in MSB order. Full scans are O(bytes scanned), with
bounded chunk scratch space rather than a complete response-sized buffer.
Native per-entry values remain no larger than 16 KiB.

Missing/corrupt chunks fail when accessed; they are never treated as implicit
zeroes. Unrelated chunks are not validated by a narrow range or an early match.
Very distant SETBIT growth still creates all required explicit zero pages and
stages them atomically. It can consume substantial memory and block its key;
sparse representation and cooperative long-operation scheduling are unfinished.
The existing page-publication/queue caps apply to replication, including growth.

PKV2 carries changed native pages and roots as one transaction. No-op updates
produce no mutation event. Old v0/v1 peers can receive only supported inline
String events; paged updates require PKV2. This is the existing custom live
replication protocol, without Redis initial sync/replay parity.

BITOP now uses [native multi-key page reduction](BITOP.md) for all eight
operators. [BITFIELD and BITFIELD_RO](BITFIELD.md) now use native field reads
and writes, while other compatibility areas listed in COMPATIBILITY.md remain. These commands do not establish
complete Redis bitmap or Redis server compatibility.

## Tests

`bitmap_storage` covers a 500-update byte model per memory engine, exact TTL,
maximum valid GETBIT offsets and rejected offsets/values, allocation-failure
rollback of native growth and legacy migration, missing-page/future-version
protection, and eight concurrent threads changing disjoint bits in common
bytes. Failed commits preserve the previous bytes and clear the old-bit reply.

An 8-MiB locality fixture limits individual allocations to 32 KiB. A GETBIT,
SETBIT and two cross-page range queries together read six pages and write one.
A full BITCOUNT plus BITPOS stopping at the second chunk reads 514 pages and
writes none. These are exact page-touch/allocation checks, not timing claims.

`bitmap_commands` verifies fixed edge cases, strict errors, binary keys,
zero-growth/expiry, pipelining, a bytearray update model, an independent bit-loop
range model and eight socket clients updating shared bytes. It also runs the
common cases against the optional Redis reference and compares generated range
replies byte for byte. Cluster tests verify routing before parameter errors.
The real source/receiver replication fixture exercises inline and paged SETBIT,
no-op writes, later COPY/RENAME, independent copies and retained TTL.

Disk-enabled `rdb_bitmap_reopen`/`wdb_bitmap_reopen` fixtures use isolated temporary
paths and three connection lifetimes to verify growth, localized changes, count,
position and exact TTL after reopen. Database dependency libraries themselves
are not sanitizer-instrumented.


## Validation (2026-10-30)

All eight configurations compiled and passed their full suites after the final
bitmap changes. No fixture failed or required a retry in this full matrix.

| Configuration | Passed | Wall seconds |
| --- | --- | --- |
| C++17 memory ASan/UBSan | 35/35 | 65.34 |
| C++11 memory Release | 35/35 | 62.50 |
| C++17 oneTBB ASan/UBSan | 36/36 | 67.74 |
| C++11 oneTBB Release | 36/36 | 59.03 |
| C++17 RocksDB ASan/UBSan | 39/39 | 72.89 |
| C++17 RocksDB Release | 39/39 | 66.10 |
| C++17 WiredTiger ASan/UBSan | 39/39 | 115.68 |
| C++11 WiredTiger Release | 39/39 | 109.58 |

There are 298 passing CTest entries across these runs. Each configured server
and the optional local Redis reference passed 1,632 common bitmap checks;
1,812 generated range replies matched exactly in each build. The reference
reports `modified=1`. Both disk backends passed the three-lifetime bitmap reopen
fixture; full suites also retain their existing recovery/replication checks.
WiredTiger configurations ran sequentially and retained their existing
180-second socket fixture limit. Core sanitizer builds use ASan and UBSan.

New source lines satisfy the 80-column constraint and new functions stay below
200 lines. Python syntax and CRLF-aware tracked diff whitespace checks pass.
These are bounded correctness and locality checks, not a Redis performance or
completeness certification. No commit or external publication was performed.
