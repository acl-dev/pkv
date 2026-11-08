# Compact page erasures: private replication protocol 5

PKV5 extends the private page/EXEC transport; it is not Redis PSYNC. The
receiver requests `PKV-REPL 5\r\n` and accepts a lower READY version. An upstream
that closes before READY is retried with versions 4, 3, 2 and 1. Reconnects
start at version 5. Existing authentication rules remain in force.

## Wire format

PKV5 uses magic `0x504b5635` and the existing five-u32, 20-byte header. Operation
17, PAGE_ERASE_GROUP, carries one owner key and 2 through 1024 big-endian u64
page IDs. IDs must be nonzero, strictly increasing and ordered with the other
changes for that owner. They need not be contiguous. A prior root declaration
is required. This operation carries no separate page-ID prefix.

BEGIN still declares physical page-change count, root count and total wire
bytes. A group accounts for multiple physical changes and one wire frame.
Singleton erasures and page writes retain their previous layout. Only a complete
validated COMMIT exposes an immutable batch to atomic database application.
Malformed, truncated and allocation-failed input publishes no prefix.

Capture prefers the existing representation. Compact encoding is a fallback
when the legacy representation cannot fit. Such batches require negotiated
version 5; sender preflight rejects older peers before writing any batch bytes.
An unsupported subscriber cannot receive this batch and must upgrade. This
transport does not add snapshots, offsets, acknowledgements or recovery across
disconnects.

On a negotiated version-5 connection, ordinary page batches retain version-4
framing. An ordered EXEC can contain version-4 page batches, version-5 compact
batches and version-4 MOVE intents; its envelope uses version 5 if needed.
Older negotiated streams keep their previous framing and minimum versions.

## Bounds and ownership

Encoded limits remain 64 MiB and 65536 frames, including BEGIN/COMMIT and EXEC
envelopes. Compact batches and complete EXEC groups additionally cap expanded
actions at 262144 and charged expanded storage at 320 MiB. Root/page objects,
owner/value bytes and object padding contribute to that storage charge. It is
an accounting bound, not measured allocator use or RSS. Receivers validate
declared metadata before reserving vectors and charge repeated owner keys before
expanding groups. Nested batches share the outer EXEC expansion budget.

Queue accounting retains encoded byte/frame limits and charges expanded storage
and actions, including in-flight sends. Its action allowance is four times the
configured frame allowance. Its storage allowance is five times configured
bytes plus 256 bytes per allowed expanded action; arithmetic saturates safely.
Workspaces and native family plans also bound expanded work. Capture, replay,
publication failure and cancellation release matching reservations.

## Validation and performance scope

`compact_erase` checks 66000 sparse erasures, a boundary write, 69 wire frames,
atomic MDB application, old-peer preflight, queue limits, malformed input,
allocation failures and mixed EXEC aggregate limits. `compact_erase_capacity`
seeds 270000 binary 1KiB members, trims to 6000 and verifies ordinary pipeline,
cross-DB EXEC, WATCH invalidation, TTL and live replica convergence. It runs
with MDB by default and oneTBB in the corresponding build.

The smaller List fill targets enabled by this transport improve selected 1KiB
and mixed-value workloads, with memory and range-read tradeoffs. Full results
and limitations are recorded in the
[performance report](benchmarks/performance-compact-erase-2026-11-03.json).
