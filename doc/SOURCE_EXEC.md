# Source EXEC commit integration

Live EXEC now routes eligible transactions through a native page workspace.
Successful queued mutations read preceding staged changes and reach the source
engine in one atomic commit, after reply objects have been constructed. Ordinary
commands retain their existing shared-admission path. EXEC retains exclusive
actor admission and frozen expiry time through staging, commit and publication.

## Commit domain and routing

Eligibility requires all database-accessing commands to refer to one logical DB
and a native backend with one atomic commit domain. Native mdb and oneTBB support
this domain. RocksDB/WiredTiger support it when that logical DB has one physical
store. Their multiple-independent-path configurations retain their previous
per-command execution behavior pending coordination/recovery support.

SELECT is evaluated in queue order. A transaction can SELECT a noninitial DB,
operate there, then SELECT another DB without accessing data there. COPY to the
same referenced DB uses the same workspace. The handler restores the selected
raw DB after commit/failure/unwinding; later pipelined commands never reuse a
finished workspace. Invalid SELECT/COPY operands retain normal runtime errors.

Transactions accessing multiple logical DBs retain the existing source path,
as do CLUSTER commands and backends without native page transactions. The
planner conservatively counts reads as DB references, too. Cross-DB COPY also
retains that path. These fallbacks still can recover a source EXEC prefix after
a crash; they do not establish coordinated commits. PKV3 receivers still reject
mixed-DB groups before any destination write.

Legacy COPY/RENAME now import enumerable old values directly into destination
pages. Source preflight checks the decoded view's native_importable capability;
repeated source names are deduplicated. Opaque/read-only extensions, old empty
Hash records and incompatible observers retain the previous path. Native commit
rejection never retries as a large opaque write. See
[legacy transfer boundaries](LEGACY_TRANSFERS.md).

EXPIRE/PEXPIRE/EXPIREAT/PEXPIREAT/PERSIST now import live legacy values into
native bounded pages inside their own page transaction, then change only the
metadata. Their legacy inputs no longer force EXEC out of the workspace.
A 16380-byte old List whose new expiry would grow its JSON past 16 KiB is
covered by a regression. Conditional no-ops keep the old encoding untouched.
See [legacy expiry migration](LEGACY_EXPIRY.md) for retry and fallback policy.

Typed page DAO migration remains supported inside a workspace. Opaque source
views can opt in with complete range/visitor accessors.
New container writes retain their bounded layouts; no new whole-container
serialization format is introduced.


## Errors, replies and observation

Queued runtime errors still permit later commands to run. Workspace staging
rejects an invalid or over-budget proposed batch without changing earlier
staging. Pending native input is limited to 64 MiB/65536 encoded frames,
including the eventual outer PKV3 controls. These account encoded input, not
all response/index/allocator memory. See [workspaces](PAGE_WORKSPACES.md).

If the final source commit fails, EXEC returns the top-level error
`ERR EXEC storage commit failed`, clears its tentative reply array and leaves
source data and WATCH unchanged. It sends no success replies for staged writes.
Later commands on that connection use the raw selected DB. This storage-failure
response is a pkv policy, not a claim of identical Redis persistence errors.
Completed SELECT operations retain their connection-local selection.

An exception before source commit discards staging while unwinding and closes
the client through the existing allocation-failure path. After successful
commit, a socket/serialization failure can still lose the acknowledgement;
committed data cannot be rolled back. Reply objects are constructed before
commit; final wire serialization and socket writes happen afterward.

Source WATCH invalidation uses ordered logical history, including creation and
deletion of an initially absent key. Source observers receive no staging events;
the original batches publish after a successful engine commit while the PKV3
capture scope is active. Output/allocation failure can close subscribers while
source data remains committed. Pure migration retains maintenance semantics.

## Source staging cost

The prepared workspace index supplies the source delta without a second root
read or command replay. Its private key lock count is one; shared databases
retain 256 stripes. View-local WATCH metadata is lazy, while the actual source
registry is initialized before concurrent serving. Network replay retains its
validation and uses the disjoint-owner fast path only when no owner repeats.

Local measurements found a significant memory EXEC overhead in the first
implementation; profiling identified repeated creation/destruction of 256 ACL
mutexes per workspace. The later changes reduce that cost, but source staging
still consumes CPU and memory. The benchmark compares local pkv snapshots,
not upstream Redis; final measurements and their limits are recorded separately.

## Verification and remaining work

`source_exec` drives the actual redis_handler over socketpair-backed replies.
It checks one physical source commit for multiple successful commands, staged
reads, runtime errors, SELECT/COPY routing, final DB restoration, failed commit
without data/WATCH changes, legacy/domain fallbacks and 260 allocation-failure
positions. It rejects any source prefix or more than one successful commit on
the eligible path.

`source_exec_rdb` and `_wdb` execute that same handler with actual disk engines.
A test subclass exits immediately before engine commit, immediately after it,
or after reading a successful EXEC acknowledgement. Another process reopens the
store and verifies all seven keys absent or all five data types readable,
including a 70-KiB String. The commit hook is test-only; no production fault
switch is installed. This proves process-crash behavior at the source commit
boundary, not arbitrary power-loss behavior or a cross-store journal.

The real TCP EXEC suite also checks staged RENAME/COPY, expiry preservation,
delete/recreate, key scans, source WATCH history and post-EXEC commands, alongside
existing all-type publication and coherent replica-read cases.

The transfer implementation passed 454 full backend-matrix tests. Later
format-retry and missing-source regressions were corrected, then all
eight configurations were rebuilt and passed 88 affected-path checks. The full
matrix was not repeated after these corrections. Exact scope/logs are in [the test record](../tests/README.md).
Earlier [local measurements](SOURCE_EXEC_BENCHMARK.md) include the remaining
memory EXEC throughput cost; they predate legacy expiry/transfer integration.

Cross-database/physical-store coordination, remaining legacy transfer
migration, initial sync,
reconnect replay, offsets, acknowledgements and Redis replication compatibility
remain open. A complete Redis server and a performance advantage over upstream
Redis are not established by this change. Persistent durability inherits each
engine's sync/WAL policy. The current RocksDB batch path uses default
WriteOptions (sync is not enabled); process-crash tests do not prove survival of
an OS/power failure. A source crash after its local commit but before publication
can also leave a replica behind: durable publication/reconnect replay remains
required.
