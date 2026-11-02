# Ordered page replay in one database

`page_sequence` owns an ordered set of immutable `page_batch` objects from one
logical database. `apply` stages their effects under the union of key locks and
calls the selected engine's atomic root/page commit once. It emits no observer
echo. The single-batch replica path shares input validation and retains its direct
staging path; it allocates no multi-batch replay state map.

This primitive now applies complete single-database PKV3 EXEC groups. See
[whole-EXEC replication](EXEC_REPLICATION.md) for framing, capture and receiver
rules. Cross-database coordination, all-path source whole-EXEC durability,
replay/reconnect recovery and initial synchronization remain open.

## Ordered semantics

A root KEEP retains the staged state. A root write replaces its metadata; a
replacement also drops the owner's earlier staged pages and selects a physical
page-family reset at commit. Root erase drops both the family and earlier staged
pages. A later root write recreates the owner with a reset, even if its incoming
write did not itself request a reset. Ordinary later writes retain earlier
resets. Each address keeps its last staged write/erase after the most recent
family reset. Empty and binary owner keys remain ordinary keys.

Every input batch is validated for sorted/unique roots and page addresses,
nonzero page ids, declared owners, bounded values, legal actions and maintenance
flags before storage access. Replay also checks each intermediate state against
the locked initial roots. A page write under KEEP on a missing or previously
erased root aborts the entire sequence, even if a later root write could make
the final flattened delta look valid. No prefix reaches storage on rejection or
allocation failure. A backend failure is returned from the single atomic commit.

Staging reads roots and incoming changed pages, without loading old page payloads
or copying an owner's whole page family. Physical root/page payloads retain the
16384-byte limit. Legacy roots already present in storage may still be large;
this primitive does not create a serialized whole-container representation.

## WATCH and input ownership

WATCH notification occurs only after a successful commit. A normal create then
erase invalidates a watcher of an initially missing key, even though the final
physical state is also missing. An internal history flag preserves that fact.
It is rejected by standalone page capture and by replay inputs; it is neither a
new wire opcode nor persisted metadata. Ordinary missing-root deletion, including
its cleanup page erases, retains the existing missing-key WATCH semantics.

Pure maintenance preserves WATCH when the destination root already exists. A
complete maintenance replacement applied to an absent destination creates a key
and invalidates its missing-key WATCH. Single and grouped replay use the same
rule. A maintenance-created key that is later deleted in the same sequence also
retains creation history and invalidates the missing-key watch.
A consistent upstream base is still needed for other maintenance
assumptions; initial synchronization remains open.

Any earlier logical write prevents a later
maintenance marker from hiding the write. The full sequence retains ordered
batches rather than serializing a flattened result that would lose this history.

Appending null, mixed-database or over-budget input leaves retained batches and
accounting unchanged. Callers assembling an EXEC must discard the whole envelope
on any append failure; they must not apply the retained prefix. This builder is
not a protocol receiver with sticky failure state.
The byte and record budgets sum the existing batches' encoded sizes, including
each nested BEGIN/COMMIT: at most 64 MiB and 65536 records. These account input
wire data, not total resident memory. The PKV3 outer envelope separately
charges its own control frames. Shared batch ownership outlives receive buffers;
raw `page_update` views are borrowed for the duration of the synchronous DB call.
Callers select the destination using `database()`. Concurrent mutation of a
builder or its borrowed input is not supported.

## Validation

`page_sequence` tests root replacement, delete/recreate, untouched-page retention,
invalid intermediate prefixes, WATCH history, maintenance, null/mixed input,
byte/record limits, failed append accounting and a model with 100 sequences of
20 ordered updates each. Allocation sweeps check that roots/pages and WATCH stay
unchanged on failure, old pages are not read during staging, and at most one
commit is attempted. A storage-rejection fixture checks retry behavior.

RocksDB/WiredTiger configurations with independent physical stores retain
their existing rejection of writes spanning stores. The complete sequence is
preflighted before any store commits. Supporting these configurations requires
the future commit/recovery coordinator; rejection is not full Redis parity.

The independent model targets physical root/page operations rather than DAO
layout validation. The native backend fixtures run the same cases against oneTBB, RocksDB and
WiredTiger. Disk fixtures close the writer process and reopen the stored result
in another process. They do not claim a new whole-EXEC crash-recovery protocol;
the existing engine crash tests continue to validate native atomic commits.

Disk fixtures also open two independent stores and verify that a sequence
spanning them returns unsupported with both data and WATCH unchanged.

The missing-owner cleanup case also exposed a oneTBB preparation bug: it rejected
page erases before checking whether the operation was an erase/no-op. Preparation
now skips those erases before requiring live owner data, while page writes still
require an existing owner. This preserves the same cleanup semantics as the
other native backends and leaves the scan index unchanged for missing owners.

The source now also uses ordered replay for eligible live EXEC commits through
native workspaces. See [source integration](SOURCE_EXEC.md). Multiple commit
domains and legacy fallback paths remain open.

Disjoint-owner sequences now reuse the single-batch staging path without a
history state map. Owner count is compared with the locked deduplicated root
view; repeated owners always retain full ordered replay. Dedicated tests cover
independent root replacement, KEEP page updates, missing-root maintenance,
missing deletion cleanup and a missing KEEP base that rejects the whole group.
