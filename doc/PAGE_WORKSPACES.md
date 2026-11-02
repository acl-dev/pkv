# Native page workspace for source transactions

`page_workspace` is a DB view for staging multiple commands against one native
source database. It holds exclusive actor admission until destruction. Ordinary
DB transactions and the existing String/List/Hash/Set/ZSet page DAOs read their
own preceding writes through this view. Only explicit `commit()` writes the
source; destruction and `discard()` abandon pending changes. Successful commit
finishes the workspace; a second commit or write is rejected. A failed commit
retains pending input and can be retried or discarded.

Live EXEC now selects this view for one referenced native logical DB and one
atomic physical commit domain. SELECT and same-DB COPY route through the view.
See [source EXEC integration](SOURCE_EXEC.md) for eligibility, replies, crash
checks and remaining fallback paths. Cross-database/physical-store coordination
and recovery are still required. Current network guarantees remain documented
in [PKV3 replication](EXEC_REPLICATION.md).

## Storage and read semantics

The workspace retains immutable bounded page batches and a sparse per-owner
index of changed page ids. Root and page indexes point into retained batches;
values are not serialized into a whole container. Unchanged roots/pages are
read lazily from the source. Native roots and pages written into the workspace
remain at most 16384 bytes. Reads can still encounter a large legacy root and
migrate it through the existing typed DAO, retaining that first-read JSON cost.
An oversized raw root write is rejected rather than stored as a new large value.

A root replacement, erase or recreation advances the owner's generation. Old
staged page slots become invisible without copying or loading the existing page
family. Later writes can reuse individual slots. Untouched pages fall through
to the source only while no reset has occurred. A deleted root exposes no old
pages; recreating it cannot resurrect either old native pages or earlier staged
pages. Index slots may remain for obsolete generations until discard; they are
bounded by the charged input history.

Staging prepares all allocating history and index operations before publishing
pointers or generations. If allocation fails, newly inserted index nodes are
removed and the preceding workspace view and accounted sizes remain unchanged.
Repeated writes reuse existing index nodes. Long owner keys are indexed once
per owner, rather than copied into every page-index address.

The default input limits include the eventual PKV3 outer controls: 64 MiB of
encoded data and 65536 frames. Each retained batch includes its own controls.
A lower byte/record limit can be supplied. Workspaces without staged mutations reserve no
controls. Logical reads can still stage legacy format migration. Exceeding a limit rejects that proposed batch and retains prior valid
staging. These are encoded input limits, not total RSS limits; history, indexes,
allocator overhead, responses and capacity slack use additional memory.

Native key scans filter staged erases and include staged writes on cursor zero.
They retain the source engine's cursor format. COUNT remains a hint; the first
page can contain all staged keys, and later native pages may repeat overwritten
keys. Legacy DB cursor APIs are unsupported. Expiry/type filtering remains in
the ordinary DAO layer and uses the actor's frozen execution time.

The source must outlive the workspace (shared ownership enforces this), and
must not be reopened or mutated directly by the owning actor during staging.
All mutation must go through its workspace view. The global exclusive gate
excludes public mutations from other actors. Nested workspace sources are
rejected. Each workspace and its DB/page callbacks belong to one actor; they
are not independently thread-safe handles.

## Commit and observation

A workspace validates each proposed command through ordinary DB/page
transactions and immutable batch capture. Its sparse index retains the initial
physical presence plus ordered logical write history. While exclusive admission
is still held, commit builds a sorted final root/page delta directly from that
index. A private source commit method preflights observer capabilities, then
calls the engine once. Source roots and changed pages are not read or replayed
again. Internal history flags never become wire metadata. Raw root writes record
actual view presence before staging, preserving native key-scan indexing.

This prepared path relies on the workspace lifetime contract: another actor
cannot enter, and the owning actor must not mutate/reopen its source directly.
Network input cannot access it. Generic `db::commit_page_sequence` and replica
apply still validate all ordered inputs and intermediate root presence. Disjoint
owners can share the single-batch staging path; repeated owners retain full
ordered replay. Both reject invalid prefixes before storage.

The private view has one key mutex, while ordinary shared databases retain 256
stripes. Its own WATCH registry is created only if its inherited WATCH API is
used; normal databases initialize their shared registry before serving actors.
Source WATCH always remains separate and is invalidated only after success.
This avoids constructing/destroying 256 ACL mutexes and an unused watch namespace
for every live EXEC. View-local WATCH tests cover registry ownership/lifetime.

An engine rejection or pre-commit allocation failure leaves source data and
WATCH unchanged. Independent physical stores retain rejection before any store
writes in the generic workspace API; live EXEC keeps the documented fallback.
No multi-store coordinator is implied.

On success WATCH invalidation preserves ordered logical history, including
creation followed by deletion of an initially missing key. Pure legacy migration
preserves an existing key's WATCH. The source then publishes original batches
in order, preserving maintenance and logical write distinctions. Observers see
no staging events. An observer allocation failure after commit cannot roll
back data; page dispatch continues to later observers, including replication.
A caller using PKV3 must also own the appropriate replication capture scope;
this primitive alone does not add a durable source replication journal.

## Live EXEC integration requirements

The connection must route every queued operation through its workspace views,
including SELECT, COPY destinations, expiry cleanup and native key scans.
Legacy expiry/PERSIST and enumerable COPY/RENAME can import bounded pages
inside this view; opaque views still require their compatible fallback.
WATCH validation precedes staging; queued runtime errors still allow later
commands to execute. Response construction must complete before a successful
source commit can be acknowledged. An exception before commit discards pending
source changes and must not publish a replica prefix.

The replication capture scope must cover the final source commit and ordered
observer dispatch. Read-only workspaces need no engine commit or group controls.
A single native engine commit inherits that engine's configured durability; a
coordinator/recovery protocol is still needed for multiple logical/physical
stores. Committing separate workspaces sequentially does not establish that
cross-database guarantee. Server-level kill/reopen and real source EXEC tests
now cover the eligible live routing path; the component tests below do not
prove that integration by themselves. See SOURCE_EXEC.md for the handler tests.

## Validation

`page_workspace` tests all five typed page DAOs with 70-KiB members/values,
read-through, overwrite/reset/delete/recreate/KEEP, missing-key WATCH history,
large legacy migration without WATCH invalidation, staged scans, discard,
finished/nested workspaces, limits and engine rejection/retry. An independent
100-step model checks roots and pages after each staged update and after commit.
Two 220-position allocation sweeps exercise multiple owners, index preparation,
reset publication, ordered native commit and retry; failed writes expose no
source prefix. Commit staging reads no old page payloads. Observer fixtures
check delayed ordered event counts and dispatch past a throwing observer.

Native backend wrappers run the same data/model/DAO cases against oneTBB,
RocksDB and WiredTiger. Disk wrappers close the writer and reopen committed
values in another process. This checks one engine commit's persistence, not
whole-EXEC server crash recovery or a new cross-database journal. Disk wrappers
also open two independent physical stores and verify unsupported commit leaves
both source values and WATCH unchanged while preserving pending workspace data.

The eight-build full matrix passed 414/414 CTests. After the final two
formatting-only line wraps, every configuration was rebuilt and passed 68/68
affected-path CTests. See [build counts and logs](../tests/README.md).
External RocksDB/WiredTiger libraries remain unsanitized dependencies.
