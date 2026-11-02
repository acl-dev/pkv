# MOVE decision journal

`move_journal` is an internal, single outstanding decision log for the planned
cross-database MOVE storage coordinator. It is compiled into the core and
exercised by
native tests and the optional [bound startup recovery](MOVE_STARTUP.md). The
command dispatcher does not create decisions; this journal alone does not make
cross-database writes atomic or implement the Redis command.
The existing [WAL barrier](DURABILITY_BARRIERS.md) supplies a separate source
persistence prerequisite.

The journal stores DB identities, a serial and a binary key, together with an
exact storage configuration identity. It stores no value or collection pages.
The future coordinator must use the already durable source for redo, keeping
all existing collection page bounds. Logging a command alone cannot substitute
for synchronizing that locked source before recording the decision.

## Ordering and failure contract

The coordinator serializes calls and holds source/target admission and key
locks. Its required order is:

1. Validate the source, absence of the target, configured DB identities and
   supported native transfer. Prepare anything that can fail before deciding.
2. Synchronize the selected source WAL under the retained locks.
3. Call `decide`: create a private temporary file, write the complete record,
   synchronize the file, close it, rename it onto the decision path, then
   synchronize the parent directory. Only then acknowledge a durable decision.
4. Apply and synchronize the target before deleting the source. Apply and
   synchronize the source deletion before completing the journal entry.
5. Call `complete` with the current serial: unlink the decision and synchronize
   the parent directory before releasing traffic into the completed operation.

Only step 3 and the journal removal in step 5 are implemented by this component.
It cannot check DB WAL state, record application, lock ownership or replication.
The [physical coordinator](MOVE_STORAGE.md) now retains source/target locks
through decision and application. Its server policy/effects adapter and grouped
replication remain unfinished. Startup binds a
stable storage identity and invokes [native redo](NATIVE_MOVE_REDO.md).

`open` takes an absolute path with an existing parent and a nonempty exact
configuration identity. The identity must describe the engine, ordered canonical
storage roots, logical DB count and applicable format/domain version. Deriving
that identity from server configuration is now supplied by the bound startup
domain; arbitrary internal callers must still supply it correctly. The journal
location itself must remain stable across restarts: a configuration identity
can validate only a record found at that location, not discover an abandoned
journal elsewhere after storage paths change. A sibling
`.lock` file retains an exclusive nonblocking POSIX advisory lock for the object
lifetime; it is deliberately not unlinked. Every native descriptor is close on
exec. Symlink journal/lock files and nonregular journal files are rejected.
Concurrent calls on one object require external serialization.

Startup validates the complete surviving record and exact identity, then
synchronizes both that file and its parent before returning `pending`. Absence
also requires a successful parent sync before returning `empty`. A caller may
not resume traffic or expiry until a pending record has been replayed and
cleared. Temporary files left by a killed predecision writer are ignored; they
are never interpreted as decisions. Automatic orphan-file cleanup is not added.

IO, corruption, configuration mismatch or allocation failures set a sticky
`failed` state. No subsequent decision or completion is accepted on that object;
its descriptors retain the lock until destruction. In particular, a rename
followed by a failed directory sync may already have published the decision.
The caller must recover by reopening, rather than assume rollback or continue
serving a partially applied MOVE. After a failed removal sync, reopening can
find either a surviving decision or no decision, so redo must be idempotent.

Serials are nonzero and strictly increase during the locked object lifetime.
Publishing while pending, completing with a stale serial, or reusing a serial
is refused without replacing the pending decision. A new owner after closing
an empty journal need not retain historical serials: the prior owner no longer
holds its descriptor/lock. The serial is a coordinator ticket, not a replicated
transaction identifier. DB-number bounds require coordinator validation; this
format only rejects identical source and target numbers.

## Internal record version 1

Integers use network byte order. No native structs or yyjson encoding are
persisted. The footer is the IEEE CRC32 provided by zlib over every preceding
byte; it detects accidental corruption and torn records, not hostile rewriting.

| Offset | Bytes | Field |
| --- | ---: | --- |
| 0 | 8 | ASCII `PKVMOVE1` |
| 8 | 4 | Source logical DB |
| 12 | 4 | Target logical DB |
| 16 | 8 | Nonzero serial |
| 24 | 4 | Configuration identity byte count |
| 28 | 4 | Binary key byte count |
| 32 | Variable | Exact configuration identity, then key |
| End minus 4 | 4 | CRC32 |

The identity is 1..65,536 bytes; the key is 0..512 MiB, including embedded NULs
and nontext bytes. The file size must match exactly, with no truncation or
trailing bytes. Lengths and identity are checked before allocating the key.
IO requests are at most 64 KiB; retaining the key is O(key length), with no
allocation proportional to its value or number of collection pages. Record
copies and temporary path allocation happen before publication. A compile-time
check requires cached decision publication after rename to be nonthrowing.

## Verification and remaining work

`move_journal` checks binary/empty/large keys, bounded IO, native permissions,
exclusive ownership, independent reopen, every-byte truncation/corruption,
trailing bytes, mismatched configuration, invalid lengths, stale serials,
short/zero/overreported IO, EINTR, injected IO failures and allocation failures
both during publication and startup. An explicit call trace checks persistence
ordering. `move_journal_crash` kills only its own native writer at ten IO and
acknowledgment boundaries, then independently reopens twice. Python also parses
pending native records with `struct` and checks CRC with zlib.

These are journal component and process-kill tests. The subsequent
[native redo helper](NATIVE_MOVE_REDO.md) now applies existing decisions with
bounded target-before-source replay and native reopen/failure checks. Online
server policy/logical-effects integration,
grouped cross-DB replication/replay, fast native memory family transfer and
command semantics still need implementation and end-to-end testing. Streams,
scripting and standard Redis replication are also outstanding compatibility
work. See [prepared memory commits](PREPARED_PAGE_COMMITS.md) for the separate
volatile preparation boundary.

## Journal-component validation

The journal revision before native redo passed **1,134 CTests** across eight
MDB, oneTBB, RocksDB and WiredTiger configurations, including C++11 and
ASan/UBSan. The two journal fixtures passed five
further repetitions on each of four ASan builds, adding **40 passing CTests**.
Each crash fixture covers ten owned writer terminations and two independent
opens per cutpoint. Allocation publication and load probes each try 48 budgets,
with both failing and successful paths covered. Existing native 4/2 allocation
bounds and 4,480 paired allocation budgets also passed the full matrix.

All **590 source fingerprints** are retained. After full testing, one test
registration list was wrapped from 82 columns to at most 80, keeping the same
line count and tokens. All 16 generated test manifests across the eight
configurations remained byte-identical after reconfiguration; the other 589
sources remain byte-identical to the full tested freeze. The 580 unchanged
parent sources include every existing production source, command registry,
codec and storage engine. Only test registration changed among prior sources;
nine new journal and test files were added. The
[validation artifact](benchmarks/journal-validation-2026-11-01.json) records
configuration, source/binary/runtime/log fingerprints and limits.

At that journal revision, 8/8 server executable hashes matched the parent.
The journal was linked by dedicated tests and was not referenced by startup.
No new ordinary-throughput benchmark or Redis superiority claim is made.
ASan uses the retained private ACL fiber runtime with stack registration;
Release uses the original installed runtime. No TSan run is claimed. The prior
unexplained RocksDB Release startup SIGSEGV remains an unresolved historical
limitation, not a fix claimed from this matrix.
