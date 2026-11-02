# WiredTiger native storage

`HAS_WT=ON` enables the `wdb` backend. Native List, Hash, Set and ZSet DAOs
use the same bounded roots and pages as the memory and RocksDB backends.
No structured command writes the whole collection as one serialized value.

Use `./build/pkv alone wdb_native.cf` for a fresh single-path example at
`./data/wdb-native`. The existing four-path `wdb.cf` is unchanged. This example
does not move existing shards. Paths retain their previous CRC32 placement and
`<configured path>/wdb` connection directory.

## Tables and binary keys

Each connection has four logged row-store tables with raw `u` key/value formats:

| Table | Content |
| --- | --- |
| `pkv_roots_v1` | `0x01 || logical key` to stored root/value |
| `pkv_pages_v1` | Owned physical pages |
| `pkv_scan_v1` | CRC32 bucket byte plus logical key to empty value |
| `pkv_meta_v1` | `schema` to `binary-v1` bootstrap marker |

The root prefix permits empty logical keys. All APIs pass explicit byte lengths;
embedded NUL bytes and empty values survive reopen. Page addresses use the same
shared encoding as RocksDB: BE64(owner byte length), owner bytes, BE64(page ID).
Page IDs are nonzero; the maximum uint64 ID is valid. User root keys cannot
collide with private pages or index entries. Physical collection pages are at
most 16 KiB, independent of large members and collection length.

The previous `table:access` used C-string `S` formats. On first upgrade, bootstrap
copies its readable records into the native root table without replacing any
already copied roots. The old table remains unchanged and becomes stale after
native writes. Binary suffixes previously lost through C-string storage cannot
be reconstructed. Downgrading to an old server would read that stale table.

The private scan index is rebuilt if the bootstrap marker is absent. Repeating
bootstrap preserves native roots and pages and replaces only the private scan
index. The marker is committed after migration/index writes with log sync.
Unknown schema markers and incompatible native table formats fail open rather
than overwrite user records. Startup errors must be resolved before serving.

## Atomicity and lifetime

One snapshot transaction commits all changed roots, pages and scan entries on a
single connection. Root replacement/delete truncates its inclusive page range
from ID zero through UINT64_MAX. Public logical-key locks serialize operations
on the same owner, including multi-key commands, while other owners can proceed.
WiredTiger truncation alone does not protect concurrent inserts into that range.

Conflicting native transactions roll back and retry up to 32 times. Only the
already staged storage plan is retried; the user callback is not executed again.
Other failures abort the transaction and return a storage error. Session leases
reset cursors and roll back active transactions before returning to the pool;
sessions/cursors close before their connection. Cursor key buffers remain alive
through the operation because WiredTiger may borrow them after `set_key`.

Writes spanning independent configured paths are rejected before mutation.
Read-only inputs on another path can participate in a single-path destination
write. General distributed disk transactions and recovery are not implemented.
SCAN visits all 256 buckets per path, hides private records and retains the
usual weak consistency under concurrent edits.

Commits request `sync=on` with logging enabled. This differs from the former
backend's unsynchronized per-record writes. The current connection uses a
128 MiB cache and 256 native session slots. Native disk calls are synchronous
and occupy their worker thread; asynchronous offloading and throughput tuning
remain future work. No throughput advantage is inferred from correctness tests.

## Validation and limits

`wiredtiger_native_storage` covers legacy LSM import, binary/empty/page-shaped
keys, maximum IDs, complete family reclamation, index rebuild, independent path
preflight and all four structured types after reopen. Raw-table inspection
checks bounded stored roots/pages. `wiredtiger_crash_recovery` kills a separate
writer during two-root/64-page epochs and checks coherent recovered roots,
pages and index in six cases. This tests process-kill recovery, not power loss.

WiredTiger-only process fixtures run command, routing and replication tests on
isolated temporary directories. Local validation uses WiredTiger 10.0.2 built
out of tree as a Release shared library, with C++17 ASan/UBSan and C++11 Release
pkv builds. The dependency itself is not sanitizer-instrumented. Replica initial
sync/replay, cross-path atomicity, power-loss validation and general Redis
completeness remain open requirements.

Final local matrix (2026-10-30): WiredTiger C++17 Debug ASan/UBSan and C++11
Release each passed 30/30 CTests. The sync-heavy socket fixture took about
161/163 seconds while the two matrices ran concurrently. The existing memory
C++11/C++17 builds passed 28/28 each, oneTBB variants 29/29 each, and RocksDB
C++17 Debug/Release 30/30 each: 234 entries across eight builds. All changed
production source was built before these final matrices. A separate unbuffered
WiredTiger command run also passed all groups and 600 floating aggregation
comparisons with the local Redis reference build. That reference checkout is
locally modified; this is compatibility evidence, not an independent stock Redis
benchmark or a throughput comparison.
