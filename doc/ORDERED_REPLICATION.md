# Ordered replication protocol 4

PKV4 remains supported. Current negotiation starts at version 5; compact page
erasures and mixed-version nested groups are described in
[compact erasure replication](COMPACT_ERASE_REPLICATION.md).

PKV4 is a private replication protocol, not Redis PSYNC. It extends the existing
bounded page/EXEC framing with family ownership transfer. The public
[MOVE command](MOVE.md) emits intents through the
[prepared source API](MOVE_SOURCE_CAPTURE.md).

## Framing and limits

All integers are unsigned big endian. Each frame starts with five u32 fields:
magic `0x504b5634`, operation, database, key length and payload length. Key bytes
then precede payload bytes. Existing page operations retain their layout.

Operation 16 is a family MOVE. The header database is the source DB. Its payload
starts with four u32 fields: target DB, flags, source root length and target root
length. The exact raw source root and target root follow. Flag 1 means the target
root exists; flag 0 means absent. No other flag is valid. An existing empty root
differs from an absent root. Source and target must differ.

One MOVE costs `36 + key bytes + source root bytes + target root bytes` on wire.
It carries no collection pages. Each root is at most 16384 bytes, keys at most
65525 bytes, and database indices below 65536. These private-stream limits do
not establish full Redis key-size compatibility.

An EXEC BEGIN has the existing 16-byte payload: u32 entry count, u32 wire record
count and u64 total wire bytes. An entry is one complete page batch or one MOVE.
The BEGIN/COMMIT envelope costs 56 bytes and two records. Groups are limited to
64 MiB and 65536 records. Capture also checks normalized workspace consumption:
a MOVE costs `64 + 2 * key bytes + root bytes` and three staging records.

## Publication and replay

Entries own immutable snapshots; changing the caller's strings after capture
does not alter the captured intent. Only a complete, validated group constructs
the admission-owning family workspace. Network waits hold no command admission.
Every move checks the exact staged prefix roots, including target presence.
Replay does not reinterpret the source decision using the replica's clock.

Memory replay preserves supported native family ownership. Tests check address
retention with 1, 64 and 4097 pages; wire size remains independent of page count.
This is structural evidence, not an end-to-end throughput comparison with Redis.
Mixed MOVE replay on unsupported disk engines fails before publishing a prefix.
Disk multi-command recovery remains pending; standalone MOVE redo is insufficient.

## Compatibility

The receiver requests the ASCII line `PKV-REPL 4\r\n` and accepts a version-4
READY frame (operation 4, empty key/payload, DB 0). Reconnect fallback retains
versions 3, 2 and 1; authentication failure is not a downgrade trigger.
Page-only groups keep minimum version 3 and identical version-3 bytes, even when
captured through the ordered API. They may also travel with version-4 framing.
MOVE groups require version 4. Sender preflight rejects an unsupported group
before writing any earlier ordinary message or committed prefix from that send.

The C++ and real TCP fixtures are `ordered_replication` and
`ordered_live_stream`. TCP tests use MDB or oneTBB memory even in disk builds;
their success is not disk receiver MOVE support. The separate public
`move_live_stream` fixture also runs actual disk primaries into an MDB replica.
See the [integration plan](MOVE_INTENT_INTEGRATION.md) for remaining work.

The transport baseline completed all-target builds and 572 selected CTests in
eight macOS configurations, before the prepared source-observation changes.
Additional TCP probes seeded a Hash with a 70 KiB field in real RocksDB and
WiredTiger replicas, then sent an earlier page write followed by a MOVE in one
complete group. Both disk replicas rejected the group with the original family
unchanged and the earlier write absent. An MDB control accepted a group built
the same way. These probes verify rejection atomicity, not disk MOVE support.

The retained [validation report][validation] records source fingerprints,
configuration-specific selections, artifacts and the known incomplete scope.

[validation]: benchmarks/ordered-replication-validation-2026-11-02.json
