# Ordered ownership replay view

`family_workspace` folds an already complete, authoritative, ordered memory
replay group into one `family_plan`. It combines page batches with same-key
cross-DB family moves. It handles intermediate reads, repeated moves,
replacement, deletion and partial updates without copying the original family.
This is a receiver-side prerequisite. Public MOVE, source command policy and
its negotiated replication encoding are still pending.

## Admission and ordered state

Construct the view only after collecting the complete envelope. Construction
acquires exclusive execution admission, retained until destruction, including
following `commit` or `discard`. Never wait for network input while holding the
view. One actor owns it, and native writes must not bypass that actor's view.
Other writers cannot enter during staging or observe a partially published
mapping. This is not a source EXEC command workspace: a staging error poisons
the whole incoming group. Source EXEC keeps its existing command-local error
and commit behavior.

Each DB/key has a stable owner record with its original raw root, current raw
root, presence and local WATCH/readiness history. Its mobile family holds an
original owner pointer plus only changed page IDs. MOVE transfers this mobile
family while local owner history stays at the key. Missing and empty roots are
distinct. Empty and binary keys are supported.

A lookup checks the current page delta first, then the original physical
family. It does not fall back to a logically erased source or an old target.
If a source key is recreated after moving its old family away, the moved
family still reads its pre-group physical origin. Immutable shared page
batches outlive every borrowed delta pointer; final materialization copies only
the bounded latest explicit changes.

Each move compares source/target presence and exact root bytes at its ordered
prefix. These are raw authoritative expectations, not Redis target-absence or
expiry policy. A receiver retains an expired family for later page updates and
PERSIST. Root equality is not a payload version or graph validation. The view
holds admission across original reads, staging and publication; its caller
must validate command policy, type/codec compatibility and the ordered group.

## Publication and effects

Only dirty owners become final edits. Original-family references are resolved
to stable pre-group owners, while fresh values have no original family.
`family_plan` prepares all native frames, page patches and destination slots
before publishing the final mapping. Its rollback/abort contract is described
in [atomic family plans](ATOMIC_FAMILY_PLANS.md).

Transient logical history still invalidates WATCH when the final bytes match
the original. Pure maintenance replacement retains its existing effect rules.
List/ZSet readiness records ordered ranks at staging time, and materialization
uses the currently registered waiter types. No incoming mutation observer echo
is generated. Unexpected exceptions after native publication begins abort
before admission unlocks; earlier failures leave native roots/pages unchanged.
Commit is terminal, and discard does not release admission before destruction.

MDB and oneTBB are supported. An existing native family can move only between
compatible engines. Independent families can share a mixed-engine group, and
fresh explicit pages can cross engines after their original family is cleared.
Disk engines, aliased DB instances and ordinary page workspaces decline this
volatile view. Durable disk group replay remains separate work.

## Bounds and allocation cost

Staged normalized input is limited to 64 MiB and 65536 records. Each move costs
three normalized records and 64 bytes plus two key lengths and its expected
root strings. These charges are staging bounds, not a wire format. Original
and current root/key cache bytes have a separate bounded counter, and cached
owners are also limited. The final family plan can reject an input that does
not fit its own bounds. None of these counters promises a strict process RSS
limit or includes allocator bookkeeping.

Existing pages are referenced without traversal or cloning. Cost depends on
owners, roots and changed pages, rather than on unchanged payload size.
Reclaiming replaced/discarded original families still costs their size. Large
legacy roots need migration before entering this bounded replay path.

The new many-target fixture exposed repeated `reserve(size + 1)` in the cold
MDB destination-slot rollback array. It now reserves once using the prepared
root count. Ordinary native frame fields and existing commit methods are
unchanged. The isolated native-slot probe reused the identical compiled test
object, compiler flags and inputs before/after this adjustment:

| New targets | Before requests / bytes | After requests / bytes |
| ---: | ---: | ---: |
| 1 | 5 / 160 | 5 / 160 |
| 64 | 312 / 42272 | 249 / 10016 |
| 300 | 1338 / 761184 | 1039 / 43584 |

Every transferred page retained its allocation address. These counts cover
C++ global allocation requests during native slot preparation, excluding setup
and verification. They do not measure every malloc, RSS or server throughput.

## Validation

Fixtures cover ordered cycles, deltas before/after moves, source recreation,
raw empty roots, binary keys, per-prefix reads, thread/fiber writer exclusion,
current-waiter List/ZSet order, maintenance, expired reads followed by PERSIST,
I/O errors, quota/capability failures and allocation/native failure injection.
Five typed fixtures use actual handler-generated String/Hash/List/Set/ZSet
page deltas, then compare roots, page IDs, CRCs and retained allocation addresses.
They do not send a new MOVE protocol over TCP.

The complete view fixture, including construction, staging, materialization,
native commit and destruction, uses the following allocations when handing an
existing family to an empty target. Setup/verification are excluded. The
1/64/4097-page cases each use 16 KiB pages; the largest has 67125248 payload
bytes, beyond the staging envelope size. No page visit/read/write occurs during
the transfer, and a zero allocation budget covers native publication.

| Engine | Requests | Requested bytes | Pages |
| --- | ---: | ---: | --- |
| MDB | 55 | 3288 | 1 / 64 / 4097 |
| oneTBB | 55 | 7416 | 1 / 64 / 4097 |

Eight configurations completed all-target builds and 490 selected CTests:
MDB 53/53, oneTBB 61/61, RocksDB 65/65 and WiredTiger 66/66 (Debug/Release).
Four Debug configurations use ASan/UBSan; WiredTiger runs were sequential.
This is a selected regression matrix, not the complete CTest suite. The source
manifest has 652 entries: 639 unchanged from the family-plan parent, three
modified and ten added. Reverting the declared integration/reservation edits
reconstructs each modified parent file exactly. Added lines fit 80 columns and
conservative function/scope bounds stay within 200 lines.

The final selected regression counts and retained evidence are recorded in the
[validation report](benchmarks/ownership-validation-2026-11-01.json).
This remains a prerequisite for protocol integration and does not establish
complete Redis compatibility or performance superiority over Redis.
