# Active key expiration

Primary services now reclaim expired roots and their private pages in the
background. Logical expiry checks on GET/TTL/other commands remain in place;
background reclamation changes when storage is freed, not whether an expired
value can be read. DBSIZE can fall without any command touching those keys.

The service uses one background thread running an ACL fiber scheduler. It visits
only opened logical databases, without opening unused disk databases. Candidate
keys come from a striped in-memory TTL index rather than repeated full keyspace
scans. Each stripe stores deadlines in an indexed min-heap. Each expiring
owner has one hash entry that owns the key and one heap pointer to that entry.
Hash entry pointers stay valid across rehashes. Refreshing a deadline adjusts
its heap position without allocating or retaining a stale entry.

Each stripe also has an atomic scheduling lower bound in a separate array.
A negative hint proves the stripe empty; a future hint lets an idle pass skip
its mutex. Persistent writes can skip removal from a proven-empty stripe.
Successful inserts publish earlier deadlines after allocation succeeds. Deletes
and later deadlines may leave an older lower bound; a due pass locks the stripe
and refreshes it from the heap minimum. This avoids publishing an exact minimum
on every TTL mutation. A stale lower bound only causes extra locking, never a
missed due key. A future stale hint can delay discovering an empty stripe until
that time; it retains no owner or deadline nodes.

The maps remain mutex protected. Relaxed hint accesses publish no map contents;
normal same-key database locking orders committed mutations. A concurrent poll
may defer a newly committed hint to its next pass, while actual deletion always
rechecks root metadata under the key lock. The separate array preserves the old
bucket layout and adds 64 atomic timestamps (512 bytes on the measured host).
See [local performance measurements](EXPIRY_PERFORMANCE.md) for gains, controls
and limits.

## Authoritative deletion and mutation tracking

A candidate is a scheduling hint. The deletion primitive reacquires normal key
locks and reads current root metadata. Renewed keys, persistent replacements,
missing keys and unreadable records are not deleted using an old deadline.
Corrupt/unsupported metadata remains untouched and loses its scheduling hint
until a later valid write or rebuild. Metadata allocation failure defers work.

Eligible keys commit through the existing root batch path, reclaiming native
page families, updating DBSIZE, invalidating the appropriate WATCH state and
publishing normal deletion notifications. A batch crossing independent physical
stores falls back to individual operations, each with another locked deadline
check. That fallback is not an all-store atomic transaction. Rejected deletes
retain their deadline hints for retry. Absent-key WATCH subscriptions survive
physical removal of roots that were already logically expired when watched.

The index is updated after actual engine commits, before external observers.
Raw writes, legacy replication, ordinary root transactions, native page commits,
key copies, native replica application and source workspace commits share these
hooks. Intermediate workspace writes do not update the source index. The final
source commit publishes its actual root state even when replication observation
is suppressed. EXEC retains exclusive admission, so the reaper cannot interleave
with a running transaction. Ordinary steady expiry work uses shared admission
and the same logical key locks as other commands.

## Derived state and failure handling

A database initially builds its TTL index from actual roots under exclusive
execution admission. Native metadata checks read no payload pages; legacy
records retain their codec parsing cost. The snapshot is published only after
it is complete. An allocation failure while updating scheduling state marks the
index dirty without changing the already committed write's outcome. A later
complete snapshot rebuild repairs it. Failed rebuilds preserve the previous
index. No persisted value format or replication wire version changes.

Initial/recovery snapshots take O(N) work and can pause command admission.
They occur on activation or index repair, not on every idle tick. The index
consumes memory proportional to expiring key names and owners. A normal work
quantum has a candidate-count budget, not a strict wall-time or byte budget;
large keys, old large JSON roots, codec work and disk commits can exceed a small
latency target. Memory/storage failures defer physical cleanup; logical expiry
continues to reject expired values. No hard reclamation deadline is promised.

Root metadata inspection now also avoids copying an inline String payload.
`string_page_codec::decode_header` validates the same PSR format without heap
allocation. Full String reads still use the owning decoder. This fast path is
shared by SCAN, KEYS, WATCH and expiration tracking. It changes no stored bytes.

## Startup configuration and lifecycle

ACL service configuration accepts:

```text
active_expire = yes
active_expire_batch = 64
active_expire_interval = 100
```

The interval is milliseconds, from 1 to 1000; the batch is from 1 to 1024.
The primary default is enabled. An idle pass waits the configured interval;
backlogged work yields for one millisecond between quanta. Failure backs off
for one second. The global record budget rotates across opened DBs; each index
also rotates its first stripe to avoid permanently favoring one hash bucket.
Pool inventory currently traverses configured logical DB slots when collecting
opened instances; it does not enumerate their keys or create new instances.

A service configured with `replica_rpc_source` does not run this worker. The
source's deletion stream owns background physical expiry there; ordinary
replica reads retain existing lazy expiry behavior. Explicitly disabling active
expiry also retains lazy expiry. These flags are startup configuration, not a
new CONFIG SET implementation. Engines without root scan support are skipped.
The worker is stopped and joined before closing its database pool or deleting
master-service observers. Existing initial-sync/replay and bounded replication
queue limits still apply; this feature does not add a durable replication
outbox.

## Verification

The C++ fixture covers deadline replacement without stale growth, hash rehash
pointer lifetime, cached deadline movement, epoch/maximum timestamps, failed
hint publication, concurrent readers/writers, cross-thread set/erase handoff,
bounded candidate batches, all five legacy/native types with
70-KiB payloads, native page reclamation, absent WATCH preservation and steady
operation without rescanning. Mutation-path tests include raw/native writes,
copy, source workspace and both legacy/native replica applications. It checks
stale candidates, future/persistent/corrupt roots, rejected-delete retries,
post-commit allocation failures, snapshot allocation failures, EXEC admission
isolation, lazy DB creation and stop/join lifecycle. Memory sweeps cover eight
post-commit and twelve rebuild allocation positions, including controls.

Native backend variants independently reopen disk data and check that both
expired roots and owned pages remain deleted. Cross-physical-store cases verify
locked per-key fallback. The socket fixture observes cleanup using DBSIZE only,
checks renewal/PERSIST and staged SET, verifies explicit
disable/passive-receiver
behavior, and confirms a real primary expiry deletion reaches a receiver that
has no active reaper. Existing full suites also exercise command/replication
behavior with the new primary default enabled.

The final eight-configuration regression passed 578/578 CTests. See the
[validation matrix](../tests/README.md#active-expiry) for standards, backends,
logs and verification limits.
