# Atomic roots and bounded physical pages

`db::transact_pages` adds the storage boundary needed by structured values. The
native `mdb` engine implements it; other engines explicitly return `unsupported`
before calling the callback. List commands now use this API through the
page-aware DAO on `mdb`; that DAO owns legacy migration and command semantics.
The low-level API alone does not interpret stored payloads.

## Addressing and ownership

A page address is `(logical key bytes, nonzero uint64 page id)`. Native page
families are separate maps, not prefixed entries in the logical key map. Empty
and binary user keys remain valid. SCAN visits logical roots only. Different
owners can use the same page id without aliasing. Each logical database owns its
own root/page families.

The caller declares every logical key before entering the callback. The DB
sorts/deduplicates the keys and acquires the same ordered fiber-aware key locks
as ordinary `transact`. Ordinary writes, replication apply and page transactions
therefore serialize on the same keys. The callback may yield, but must not
recursively access the DB or retain the transaction view. Page operations cannot
address an undeclared key. Reads return owning bytes, valid after yielding and
after the transaction ends. The view itself is confined to the callback/thread.

`roots()` exposes staged root entries. `lookup`, `put` and `erase` address pages;
reads see staged writes/deletes. Deleting the staged root hides all its pages
inside that transaction. Missing/error reads leave the output string unchanged.
A failed backend page lookup poisons the transaction and returns `error`, even
if the callback ignores it. Invalid addresses, zero ids, oversized values and
pages staged without a surviving root abort the transaction. A callback returning
false aborts; an exception releases locks and leaves stored data unchanged.

New root values and page payloads are each limited to 16,384 bytes. Existing
larger legacy roots can still be read for later migration. This interface does
not interpret a root or validate List links/counts: typed storage code must use
its format codec and enforce those semantic invariants. Page ids are supplied by
the caller; an allocator/counter must be updated in the same root transaction
before List commands can use this facility.

## Native commit

`mdb` locks the affected native shards in sorted order. It prepares replacement
strings and journals new root, owner and page nodes. Allocation failure rolls
back every newly inserted node, including newly created page-owner entries.
Existing values remain unchanged until preparation/insertion succeeds. Commit
then uses nonallocating swaps/deletes while all shard guards remain held.

The commit copies only touched values. It does not clone a whole page map to
change one page. Page reads are also lazy: opening the transaction reads its
roots, and only explicit page lookups copy page payloads. Transaction staging
uses memory proportional to the requested changes; this is not a total server
memory or maximum-command-size policy.

Ordinary root overwrite/deletion automatically reclaims that owner's pages,
including native multi-key batches and replica writes/deletes. A page transaction
preserves untouched pages while replacing metadata by default, and reclaims all pages when
it erases a root. Recreating the same owner cannot inherit its deleted pages. `reset_pages(owner)`
also discards earlier staged pages, hides the previous family and selects an
atomic clear with a surviving root write. Native commit swaps the old map into
its rollback journal and restores it on failure. PKV2 ROOT_REPLACE carries the
same operation; see [List storage](LIST_STORAGE.md).
Reclamation currently runs synchronously under native locks; asynchronous
reclamation and latency under very large deletes still need work.

These commits are atomic in native memory, not durable transactions. No disk
reopen/crash-recovery guarantee follows from this implementation.

## Observer guard and remaining integration

An observer must explicitly support page transactions before local page writes
can commit. Empty or incompatible dispatchers still return `unsupported` before
commit. The replication observer now supports native page events and validates
their bounded wire representation in that preflight. After a successful commit
it queues one atomic event instead of plain root SET/DEL notifications.
Publication failure disconnects subscribers; it cannot undo a committed write.

`replica_apply_pages` remains the observer-free path for completed received
batches, preserving the same logical locks and native atomicity. The live service
now negotiates and routes these events as described in
[Page replication](PAGE_REPLICATION.md). List commands now use these events.

List traversal, ID allocation, migration, COPY/RENAME and TTL routing are
integrated in [Packed List storage](LIST_STORAGE.md). Root bytes alone never
transfer page ownership. RocksDB now supplies per-physical-instance native
page transactions; see `ROCKSDB_STORAGE.md`. WiredTiger supplies snapshot
root/page/index transactions within one connection; see `WIREDTIGER_STORAGE.md`.
Older memory engines still need native implementations.

## Verification

`page_storage` checks binary/empty owners, hidden pages, read-your-writes,
root/page bounds, owning reads, callback aborts/exceptions, failed backend reads,
unsupported engines and the observer guard. Fault injection sweeps all staging
and native commit allocation points for a transaction combining replacements,
insertions and deletions across several owners. Every failure must retain the
original roots/pages and leave no new pages reachable.

Four threads each run four fibers and yield inside multi-key callbacks, checking
800 consistent root/page increments. Competing ordinary/replica writers must
wait for page transactions and then reclaim their pages. A format/storage test
stores a 1 MiB member in independent bounded List pages with a 40-byte root and
exact expiry. Allocation accounting compares a one-page update on owners with
10 and 5,000 pages; permitted allocation does not grow with the full page index.

The unsupported-engine fixture also constructs, writes, reads and destroys the
existing AVL backend. It exposed a duplicate table release: `acl_mdb_free`
already owns table destruction, so `mdb_avl` must not free the table separately.

## Read-only source page copying

`copy_pages_to` adds coordinated source/target page views. Same-DB copies reuse
one sorted-key transaction; cross-DB copies hold both key locks ordered by DB
identity. The source root bytes/actions/reset flags and page changes must remain
unchanged. Callback failure, source mutation or target failure cannot publish
partial destination data. Only the destination's commit and observers run.
`list_store::clone_to` uses this primitive without whole-List materialization.
Opposite-direction concurrent copies, source mutation attempts and allocation
failures are covered by `list_edits`. Command-level COPY now uses this primitive for paged Lists.


`replace_pages(owner, last)` seals a complete staged family with write IDs
1..last, rejecting holes/deletes before accepting its reset. It discards staged
changes above last and marks the old family for clearing at commit. Unlike
`reset_pages`, it retains the new staged output. The caller still must write a
bounded root; otherwise existing validation rejects the transaction. An ignored
seal error poisons the transaction. String BITOP uses this only after consuming
all old input chunks, including source/destination aliases.
