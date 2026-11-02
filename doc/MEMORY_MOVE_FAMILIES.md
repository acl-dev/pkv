# Memory MOVE page-family ownership

`move_memory::run` now prepares both native memory engines before transferring
an entire private page table. MDB transfers its ordered page map; oneTBB
transfers its hash page map. Each page's ID, bytes and allocation remain intact.
No traversal, per-page lookup, reencoding or page-change vector is needed for
the handoff. Root metadata and the absolute deadline are copied as bounded
metadata, independently of the collection's payload size.

This is an internal cross-DB component, not a Redis MOVE handler. The server
policy, observer/replication adapter and command registration remain unfinished.
In particular, a root-only page event would omit the transferred payload and
must never be substituted for the missing paired publication.

## Preparation and publication

The caller supplies both policy and completion callbacks. The policy receives
the same bounded, immutable owner/root view types as the
[disk storage coordinator](MOVE_STORAGE.md). It must validate logical expiry,
target absence, codec compatibility and command restrictions, then prepare its
observer/publication state. The owner is copied before locks are acquired;
mutating the caller's string cannot redirect the operation. Views remain
borrowed through the callbacks and must not be retained after return.

The component acquires exclusive execution admission and both key locks in DB
address order. It checks matching family-transfer capabilities before invoking
policy. MDB-to-oneTBB, workspace, same-DB and invalid-identity calls decline.
Unopened supported engines are errors. There is no sequential write/delete
fallback for an unsupported pair.

Each native engine then prepares its root changes under its existing locks.
A target replacement first detaches its old page table into normal rollback
state. A missing MDB target owner is allocated and entered in the target
frame's rollback bookkeeping before creating the transfer handle. Source pages
remain untouched during this phase.

A new cold `prepared_transfer` handle contains only two map pointers. Its
`publish` swaps the tables; the two native batches then publish their already
prepared roots. The handle is discarded without any swap if preparation fails.
It never dereferences its pointers during destruction, since source publication
may erase the now-empty source owner. The two current map aliases use compatible
standard allocators; a compile-time check requires a nonthrowing map swap.

All allocation and lock acquisition finish before this native publication
sequence. Ordinary native commit frames gain no transfer pointers or scratch
buffers. Their existing gather, prepare, publish and rollback bodies remain
unchanged. Additive cold-method/header reconstruction checks retain that proof.

After physical publication, local WATCH, expiry and generic/List/ZSet readiness
effects are published in both DBs before the mandatory completion callback.
Completion supplies the paired observer/replication event; the component does
not automatically call `on_pages` with an incomplete target image. Unlike the
disk component's current completion contract, the memory completion callback
must not repeat the already published local effects.

Before native publication, allocation errors or exceptions roll both engines
back and return an error. Once native publication starts, unexpected callback
or logical-publication exceptions abort before releasing either logical key
lock. Returning an ordinary error after a committed ownership move would let
callers treat committed data as rolled back.

## Complexity and limits

The source-family handoff performs a constant amount of work regardless of its
page count. The complete call still includes bounded root metadata, key bytes,
index preparation, local effects and the caller's publication work. Reclaiming
an overwritten or expired target costs that old family's size; observer/network
work may also scale with the payload. This is not a claim that every aspect of
MOVE is constant time.

Native bounded root lookups reject values above 16 KiB before copying them.
Legacy oversized roots still need migration. Memory page visitors expose a
borrowed per-page reference under the caller's admission/key locks, with a
16-KiB/nonzero-ID envelope check. MDB releases its shard spinlock before invoking
a visitor; the logical key lock pins the mapped page table and hash rehashes
preserve its address. oneTBB retains the record pointer after releasing its
accessor. These visitors support inspection and future publication; ownership
transfer never invokes them.

The physical family is trusted as a native object. The transfer does not walk
and validate every type-specific page reference. It preserves source bytes and
allocation identity rather than repairing corrupt graphs. It provides no disk
WAL or restart durability; volatile engines still decline the disk coordinator.
No per-key storage fields or persistent format versions are added.

## Evidence

The allocator probe counts C++ global `new` requests for an initially empty
target. Source families contain 1, 64 or 2048 pages, up to 32 MiB. Preparation,
handoff and local effects are included; fixture construction and verification
are excluded. MDB uses 19 requests/624 bytes and oneTBB uses 19 requests/4864
bytes at all three sizes. These are allocation/work measurements, not a server
throughput benchmark or a comparison with official Redis.

A zero allocation budget is armed after both native frames and the handoff are
prepared, then released only after both native publications. Root bytes, CRCs,
page IDs and each page's data address must match after forward and reverse
transfers. Hash/List/Set/ZSet cases transfer 48-KiB blobs or members and then
mutate the result. Failure budgets exercise both frame orders and new/expired
targets, including removal of an old five-page target by a three-page source.

Additional checks cover oversized-root rejection before copying, visitor stop,
caller key mutation, unsupported pairs, native preparation failure, concurrent
writers and WATCH. Completion observes generic notifications in both DBs and
List readiness at the target. A frozen command clock followed by active expiry
verifies that the transferred deadline is indexed without a scan/rebuild.
Owned subprocesses test rollback before publication and SIGABRT after it.
Inline empty/binary values and empty/binary keys also transfer forward and
backward, including replacement of an expired five-page target.

Eight configurations passed 390 broad selected checks. An exact whitespace
wrap and a test-only extension were followed by 60 final selected checks;
these are separate recorded source states, not a full CTest matrix. The eight
server instruction sections match across that wrap. The final freeze contains
622 source files, with 606 unchanged from the prior coordinator baseline.

See [scoped validation](benchmarks/family-validation-2026-11-01.json) for source
states, build/runtime/log fingerprints, allocation results and test scope.
Public MOVE, grouped cross-DB observer/replay integration, standard Redis
replication, scripting and Streams remain outside this increment.


[Memory cross-DB replay](CROSS_DB_REPLAY.md) now prepares all received native frames
before publishing any. The private v3 receiver supports MDB/oneTBB groups;
public MOVE, its source publication and durable disk groups remain
unintegrated. Single-DB replay retains its existing path.

[Atomic memory page-family plans](ATOMIC_FAMILY_PLANS.md) now provide a cold
native publication primitive for final family permutations and page patches.
The ownership-aware transaction view, source publication and negotiated wire
integration still need to use it; existing v3 replay remains unchanged.
