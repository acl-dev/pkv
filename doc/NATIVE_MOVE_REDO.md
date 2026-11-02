# Bounded native MOVE redo

`move_recovery::run(pool, journal)` now applies an already committed MOVE
[decision journal](MOVE_RECOVERY.md) to RocksDB or WiredTiger instances. This is
an internal startup recovery entry point, now called by the optional
[bound server startup path](MOVE_STARTUP.md). Public MOVE, server online policy/effects integration, memory family transfer
and grouped cross-DB replication remain unfinished. The internal
[retained storage coordinator](MOVE_STORAGE.md) now shares its physical phase.

The caller must open the journal with the exact storage configuration identity
and construct the corresponding DB pool. The recovery helper does not derive
canonical storage paths from native objects or prove that an arbitrary pool
matches that identity. Pool size, distinct DB objects, successful complete
initialization and native disk streaming/WAL capabilities are checked before
any native mutation.

## Required lifecycle and decision contract

Recovery runs before clients, expiry workers, subscriptions, replication
receivers or observer capture can access the databases. It emits no ordinary
observer, WATCH or readiness effects and must not be used to execute a live
command. The server now supplies this boundary when `move_journal_path` is
enabled and the storage domain has been validated and bound.

A valid decision means that its creator validated a present source and absent
target, retained their locks, and synchronized the source WAL before making
the decision durable. Recovery does not retrospectively prove those conditions.
The test writer creates decisions in an isolated process with no concurrent
actors; that fixture is not an online coordinator implementation. In particular,
calling public `sync_key`, dropping its locks, creating a decision and then
calling recovery would not implement concurrent MOVE correctly. The retained
storage coordinator now supplies the physical lock/WAL/decision ordering; the
server policy, logical effects and replication adapter are still required.

## Replay ordering

The helper takes exclusive execution admission and both key stripes in DB
address order. It accesses the engine hooks directly; recursively entering a
public DB wrapper while already holding the same key mutex would deadlock.

1. Read source and target roots through bounded native lookups. Both missing,
   an IO error or a root over 16 KiB is fatal. Metadata is opaque; expiry is
   retained byte for byte even when the deadline passed during the outage.
2. Synchronize the source physical WAL under its retained stripe, including
   the missing-source case from an interrupted prior source erase.
3. If the source survives, clear the target root and private family, iterate
   source private pages, and commit each destination page separately. Publish
   the copied root and scan index only after every page was written.
4. Synchronize the target WAL before erasing the source root, pages and index.
5. Synchronize the source WAL again before unlinking and directory-syncing
   the journal decision. Only then return from recovery.

When source deletion already survived, a present target is retained: its page
stream must satisfy native envelope limits, both WAL domains are synchronized,
and the decision is cleared. Recovery can therefore be interrupted and replayed
again after target publication or source deletion. It never restores a prior
target or claims that a durable decision rolled back.

A pending decision, failed journal, invalid DB mapping, IO failure, unsupported
engine or allocation exception causes process abort. Once both key locks are
held, a fail-closed scope aborts during exception unwinding before either lock
is released. Before that point recovery has made no native changes. No partial
result is returned to a caller that could resume traffic. Diagnostic callbacks
run under the locks and cannot access the DBs or journal themselves.

## Bounded storage traversal

The new optional `dblookup_bounded_root` and `dbvisit_pages` hooks default to
unsupported/error for other engines, including volatile and staged workspaces.
They are protected interfaces for the recovery coordinator, not public staging
commands. Complete engine initialization is required before they are selected.

RocksDB uses a pinned root result, checking its size before copying it into a
PKV string. A private-column-family iterator uses the exact length-prefixed
owner range, a stable upper bound and `fill_cache=false`. WiredTiger borrows an
idle native session, searches the same owner range and checks borrowed item
sizes before copying values. Every visited address must contain exactly one
nonzero 64-bit page ID after its owner prefix; every value is at most 16 KiB.
Callbacks may stop with `aborted`; IO/format failures return `error`. Cursor and
session lifetimes remain bounded and native exceptions cannot leave a cursor
position borrowed by a later reader.

PKV retains constant-count key/root buffers and one staged private page at a
time, rather than collecting the collection's payload in a map or vector.
Its staging is O(key bytes + bounded roots/pages), independent of the number
of pages. This is not a bound on native engine cache, decompression or allocator
RSS. No typed payload decoder or yyjson serialization is used by the copy.
Ordinary native commit functions and WAL settings remain unchanged. WiredTiger
still synchronizes each ordinary page commit; batching recovery IO is future
performance work, and no recovery throughput gain is claimed here.

Existing old-format roots above 16 KiB are rejected. A future online decision
creator must first validate and, where required, migrate legacy data into native
bounded pages before fencing and deciding. The helper checks physical envelope
constraints; it does not validate every type-specific page reference or repair
media corruption. It depends on the engine/filesystem WAL durability contract.

## Tests

The native fixture checks empty/binary keys, both configured physical
partitions, already expired absolute deadlines, orphan target cleanup, exact
root/page/index results, early stream cancellation and an 8-MiB paged String.
Probe counters require each recovery page commit to carry at most one 16-KiB
value. Hash, List, Set and ZSet cases include 48-KiB fields or members, compare
root bytes plus streamed page counts/checksums, read the recovered values, and
mutate them again without damaging the recovered blobs.

Eight process-kill cutpoints cover the source fence, target clear, copied
pages, target publication/fence, source erase/fence and completed decision.
Seventeen injected failure paths include read/stream/write/sync failures,
failures after native commits, uncertain journal completion, exceptions, aliased
DBs, volatile targets and unopened targets. Each requested failure must emit its
own injection marker and terminate with SIGABRT before a clean process replays
the pending decision. Oversized roots/pages, zero page IDs, invalid DB numbers
and missing source/target pairs must repeatedly refuse recovery while retaining
the unchanged journal. Sanitizer diagnostics are rejected even for child
processes whose termination is expected.

These are actual native engine reopen tests with injected boundary failures and
owned process termination. They do not simulate power loss or every possible
device failure. They do not prove a complete public MOVE implementation,
standard Redis replication, remote atomic replay or O(1) memory transfer.

## Native positioned-read error correction

A native cursor probe found a boundary that the first regression selection did
not cover: successful positioning followed by `get_key`/`get_value` returning
`WT_NOTFOUND`. Three lookup paths reported a missing key, and page/key traversal
reported successful end of input. During redo an incomplete page traversal
cannot safely be treated as complete. Ordinary reads and SCAN also need a
storage error when a positioned record cannot be read.

The corrected code distinguishes the operation that returned the status:
`search`/`search_near`/`next` can establish genuine absence or end, while any
nonzero positioned getter result is an error. Existing WT root/page lookup,
scan and key visitation helpers were corrected along with the new bounded root
and page stream. Healthy reads and native commit/WAL settings are retained.

`wt_read_errors` uses a real native connection and real successful positioning.
It injects getter failures afterward, restores all intercepted cursor/session
functions on close, checks 21 failure cases, preserves genuine missing results,
and verifies normal reads after removing the hook. An immutable pre-fix native
probe returned seven wrong classifications for `WT_NOTFOUND`; the corrected
probe returns errors for all 21 cases. EIO and WT_ROLLBACK cases are included.
These are API-boundary failures rather than physical device fault simulation.

## Recorded validation

The initial eight-profile selection passed **298 CTests**: MDB 30+30, oneTBB
37+37, RocksDB 41+41 and WiredTiger 41+41. It used 599 frozen source files.
After the native cursor probe exposed the error classification gap, both WT
profiles were rebuilt and reran the same selection plus the new cursor test,
passing **42+42 further CTests** with the correction. The selections and source
states are recorded separately in the
[validation report](benchmarks/redo-validation-2026-11-01.json).

The current fingerprint set contains **600 sources**, including ten new source
and test files. Relative to the first selection, 596 existing source bytes are
unchanged; two WT-only implementation files and test registration changed, and
one native WT test was added. The six non-WT executables and core archives
retain their exact tested hashes; the late production changes are inside their
inactive `HAS_WT` branches. The corrected WT selections retain the same existing
commands/properties and add one test. This is the scope of the recorded checks.

Compared with the preceding journal revision, 582 parent sources remain
unchanged. Removing the new cold declarations/methods and test registration
reconstructs seven parent files exactly. The additional existing-source change
is the explicit WT read-error correction. The report retains both source states,
configuration/runtime/binary/log hashes, before/after native probe hashes and
per-profile test names. ASan builds use the private ACL archive with registered
fiber stacks; Release builds use the original installed archive. No TSan or
new throughput experiment is claimed. The historical unexplained RocksDB
Release startup SIGSEGV is not a fix claimed from these passing checks.
