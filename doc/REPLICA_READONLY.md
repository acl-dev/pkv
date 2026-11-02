# Replica client-write admission

A service configured with `replica_rpc_source` now rejects client write
commands by default. Primary services continue to accept writes. Set the PKV
startup boolean `replica_read_only = 0`, or use `CONFIG SET replica-read-only
no`, to explicitly allow replica-local writes. Runtime CONFIG also accepts
the Redis alias `slave-read-only`. Both names are returned by CONFIG GET
patterns and reflect the same policy. Canonical and alias parameters may
appear together; the last value wins. Duplicate occurrences of the same
case-insensitive name retain CONFIG's duplicate-parameter error.

Command registration caches each root's @write classification from reference
metadata. Admission checks the cached flag, avoiding a catalog lookup for
every ordinary request. Authentication, arity and ACL checks retain priority.
Denied writes return `READONLY You can't write against a read only replica.`
and mark an active transaction failed. This applies to all 84 currently
supported write commands, including operations that would make no changes.
Read commands with maintenance writes, such as PFCOUNT's cardinality cache,
remain allowed. BITFIELD_RO remains available; BITFIELD is a write command.

EXEC checks the queued commands at admission. If the policy became read-only
after a write was queued, EXEC discards the transaction and watches and returns
an EXECABORT error containing the READONLY reason. A transaction admitted while
writable retains that admission even if a queued CONFIG SET enables read-only
before a later queued write. Internal private RPC replay bypasses client
admission and continues updating the replica.

Policy reads/writes use existing execution admission. CONFIG changes hold
exclusive admission; ordinary write checks hold shared admission. Standalone
connection controls retain their independent path. Primary GET adds no catalog
lookup or replica-policy function call.

`replica_readonly_commands` compares 131 observations with an isolated reference
replica: every supported write root, reads and HLL cache maintenance, CONFIG
aliases and validation, queue failures, policy changes between MULTI and EXEC,
policy changes inside EXEC, watch cleanup and an explicitly writable startup.
The existing active-expiry and PKV2 replica fixtures explicitly select writable
mode for replica-local seeds, then restore read-only mode for receiver
assertions. The outgoing subscriber fixture explicitly enables writable mode
while generating local mutations, then restores read-only mode. Their original
expiry, atomic visibility and replication checks remain.

This supplies client-write policy, not complete Redis replication. It does not
add REPLICAOF, promotion, initial synchronization, PSYNC or writable-replica
local-expiry tracking. The existing secondary expiry and private RPC behavior
remain in force. Full Redis compatibility remains an active goal.

Historically, the expanded radius revision passed a 942-test, eight-configuration matrix,
including ASan/UBSan Debug runs. The report records 141 source fingerprints;
Release benchmark and full-matrix binaries match. The final CMake-only line
wrap was verified to change diagnostic line numbers alone. See
[radius validation scope](GEO_RADIUS.md) and [validation evidence](
benchmarks/georadius-validation-2026-11-01.json).

Blocking List commands recheck replica write policy after a real producer wake
and before modifying storage. A client that became read-only while waiting
receives READONLY and leaves the value available. The preceding List revision passed 986/986 tests. Current evidence,
including blocking ZSet admission and the 1008-test matrix, is linked from
[blocking sorted sets](BLOCKING_ZSET.md).

The subsequent queue-index revision passed 1024/1024 tests with the same
write-admission inventory. Current evidence is in [queue validation](
QUEUE_SCALING.md).
