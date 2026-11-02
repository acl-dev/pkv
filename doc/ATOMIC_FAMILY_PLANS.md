# Atomic memory page-family plans

`family_plan::apply` prepares a final ownership mapping across memory DBs and
keys before publishing anything. It supports permutations, moved-family page
patches, fresh replacements and erased owners in one native decision. This is
a prerequisite for a future MOVE-aware transaction view. It is not a public
MOVE command, a new replication wire format, or that transaction view itself.

## Input and identity

Each `family_edit` names a DB/key, its expected pre-group presence and bounded
root bytes, the final root action/value, and either an original-family index
or `SIZE_MAX` for a fresh family. An indexed original must have existed before
the group and can have at most one final owner. This transfers ownership; it
does not alias a family into multiple keys or implement COPY by shared mutation.

All DB IDs, duplicate DB/key names, family indices, capabilities and pool aliases
are checked. Every expected root is compared under exclusive execution admission
and sorted, deduplicated logical key locks. Missing/changed roots abort without
publication; native read errors return an error. Pool factories can still have
ordinary DB-opening side effects before a later failure.

Expected root equality is not a payload version or a full-family checksum.
A staging view must retain exclusive admission across its original reads,
staging and application; a replay caller must own the complete ordered
authoritative history. This coordinator's own admission starts at `apply`.
It cannot validate an arbitrary stale client snapshot from root bytes alone.

The caller supplies an already validated final physical plan. Command expiry,
target-absence policy, codec compatibility, ordered prefix validity and full
type-graph validation
remain the responsibility of the future transaction/command adapter. In
particular, received authoritative state must not be discarded just because
its absolute deadline has passed on the receiver. Replica client reads retain
such pages for later updates, as described in [replica retention](
REPLICA_READ_RETENTION.md).

Final root writes may overwrite an old physical family, as needed when an
ordered transaction first deletes or replaces that target. The primitive
therefore cannot be used as a Redis MOVE policy check by itself. Repeated MOVE
and intervening mutations must first be folded by an ownership-aware view.

## Detach, prepare, publish

Cold native hooks detach each touched page map into an owning rollback handle.
MDB swaps its ordered map; oneTBB swaps its hash map and pins its original
record. Untouched page strings and nodes remain in their existing allocations.
No source-family visitor, per-page lookup, serialization or clone is needed.

Handles stage explicit page writes by allocating only new nodes and bounded
replacement strings. Replacements and deletions remain deferred. Rollback
removes newly inserted nodes and swaps each original map back, without copying
or reallocating its original pages. Fresh families use independent empty
handles. A discarded original remains owned until a successful final decision.

The coordinator prepares final root-only native frames in DB-address order.
Each frame then prepares empty destination slots for the selected families.
Every frame, destination slot and transfer handle exists before publication.
Native frames are destroyed before detached handles on every rollback path;
DB pins and outer key guards outlive both.

Publication applies prepared page patches, swaps the selected maps into their
final slots, then publishes every native root frame. These steps allocate no
C++ storage. The source/destination maps are private under exclusive admission
throughout. After native publication, local WATCH, expiry and generic/List/ZSet
readiness effects are published before unlocking. Caller-supplied readiness
ranks preserve the required final-ready key order within each DB; maintenance
and forced WATCH-history flags retain their existing meanings.

An exception before native publication rolls the whole plan back. An unexpected
exception once publication begins aborts before key/admission guards unlock;
it cannot return a normal rollback error after committing a prefix. Incoming
plans emit no mutation observer events. Primary paired observer publication
must be prepared and integrated separately; root-only page events remain
invalid for nonempty moved families.

## Limits and cost

MDB and oneTBB groups can coexist when their families stay within compatible
engines. A cross-engine family transfer is rejected before detachment. Disk
and ordinary workspace instances decline this volatile primitive. Disk groups
still require their own durable decision/recovery protocol.

There are at most 65536 edits plus explicit page changes. Each root/page value
is bounded to 16 KiB and each key to 65525 bytes. Counted input string bytes
(key, expected/final roots, page owner and page value bytes) total at most
64 MiB. These are input bounds, not an encoded-wire or whole-process RSS quota.
Referenced existing page families are not charged as copied payload, so a
family larger than the private v3 envelope can still be transferred. Oversized
legacy roots require migration before this bounded native plan can be used.

Per-family handoff work and storage are independent of that family's page
count. The overall plan also includes key/root validation, sorting, native
index work and explicit changed pages. Reclaiming an unreferenced old family
still costs its size. This is not a claim that every MOVE or complete EXEC
would have constant time, nor a server throughput claim. Ordinary native
commit-frame fields and their gather/prepare/publish/rollback implementations
remain unchanged; the new hooks and overloads are additive cold paths.

## Regression and lifetime evidence

The first runtime test found a new handle-lifetime bug: an owning native frame
borrowed a temporary empty page vector. ASan reported stack-use-after-scope and
the Release test also crashed. The cold per-DB state now owns that empty vector
until after its frame is destroyed. Retained before/after binaries use the
identical test object and flags. The failing and corrected source states and
logs remain distinct; the intermediate failures are not counted as passes.

Fixtures check three-DB cycles, same-DB empty/binary keys, 300 owners sharing
logical stripes, modified/inserted/deleted pages, fresh/missing targets,
capability rejection, expected-root mismatch, unopened/aliased DBs and native
errors. Allocation budgets exercise both successful publication and strict
root/page-ID/CRC/allocation-address rollback. Other tests cover typed large
Hash/List/Set/ZSet values, concurrent writer exclusion, WATCH, readiness rank,
maintenance, expired authoritative data and read-guard bypass. Subprocesses
check rollback before publication and SIGABRT after publication starts.

The raw-family measurement uses 1, 64 and 4097 pages, each 16 KiB. The largest
family has 67125248 payload bytes, beyond 64 MiB. Pointer/CRC checks verify the
handoff, while a zero allocation budget covers native publication. C++ global
`new` counting excludes setup and verification and does not cover every malloc,
allocator arena or RSS byte. No power-loss or TSan result is claimed.

## Final validation scope

Eight configurations completed all-target builds and 474 selected CTests:
MDB Debug/Release 51 each, oneTBB Debug/Release 59 each, RocksDB Debug/Release
63 each, and WiredTiger Debug/Release 64 each. Four Debug builds use ASan/UBSan
with the fingerprinted fiber runtime. WiredTiger configurations ran separately.
This is an expanded selected regression matrix, not the full CTest suite.

For the empty-target handoff fixture, including preparation and local effects,
C++ global allocation measurements were identical at all three family sizes:

| Engine | Requests | Requested bytes | Pages |
| --- | ---: | ---: | --- |
| MDB | 42 | 2040 | 1 / 64 / 4097 |
| oneTBB | 42 | 6168 | 1 / 64 / 4097 |

The frozen source manifest has 642 entries: 625 unchanged from the preceding
guard baseline, six modified and eleven added. Reverting just the declared
additions reconstructs every modified parent file exactly, including ordinary
native commit fields and method bodies. Added code lines fit 80 columns and
conservative non-namespace/non-DB-class scope bounds stay below 200 lines.
No ordinary server throughput was remeasured for this unintegrated cold path.

The [scoped validation report](
benchmarks/family-plan-validation-2026-11-01.json) records exact selected names,
source/build/runtime/log fingerprints, allocation rows, retained binaries and
the identical-object before/after lifetime regression. Production code stayed
fixed after that lifetime correction; later additions extended tests only.
