# Public MOVE

`MOVE key db` transfers an existing key from the selected logical DB to another
DB without changing the connection's selected DB. It returns 1 on success, or
0 if the source is absent/expired or the destination already has a live key.
Absolute expiry is preserved. Expired destinations are reclaimed first.
Same-DB and invalid DB arguments return errors. Cluster routing precedes the
cluster-mode rejection, matching the command's existing routing convention.
See the [Redis command specification](https://redis.io/docs/latest/commands/move/).

## Data and publication

The command supports native MDB, oneTBB, RocksDB and WiredTiger families.
List, Hash, Set and ZSet values remain in bounded private pages. Native memory
families transfer page-table ownership without enumerating or copying payload
pages. Disk transfers stream bounded pages through the retained native MOVE
coordinator. Existing legacy JSON values are imported into native pages before
transfer; newly written collections are not serialized as one large value.
Moving an already native memory family has bounded metadata work; legacy import
and disk transfer have costs proportional to the data they must process.

The dispatcher takes exclusive command admission directly. MOVE inside EXEC
uses its existing exclusive admission and declines the old single-DB write
workspace, so preceding writes are visible. EXEC retains command-local errors
and commits: a later error does not undo earlier successful commands.
WATCH invalidation, expiry-index migration and blocked-reader readiness occur
for both DBs at successful publication. No-op moves preserve live-key watches.
The ACL key specification covers the source key; the DB argument is not a key.

The paired observer prepares an immutable bounded ownership intent before the
physical transfer. Standalone MOVE emits one ordered EXEC group; MOVE inside
EXEC joins preceding and following page edits in command order. Native page
contents are not duplicated into the intent. Queue preparation failure prevents
the transfer; publication failure after commit invalidates affected subscribers.
See [source capture](MOVE_SOURCE_CAPTURE.md) for allocation and budget limits.

## Native journal configuration

The default `move_journal_path = auto` binds native storage to a decision file
named `.pkv-move.decision` in the first canonical configured storage root.
MDB and oneTBB treat `auto` as disabled and create no journal. An explicit
absolute journal path remains supported for native storage. Its parent must
already exist. Use a stable dedicated control directory when configuring one.

`off` disables the journal only for unbound storage. An empty configuration
value is ignored by ACL and retains the default `auto`; use `off` explicitly.
Actual native transfers then return an error; absent-source and occupied-target
no-ops still return 0. A previously bound pool cannot disable or change its
journal identity. Preserve the markers, locks and decision file with storage.
See [startup recovery](MOVE_STARTUP.md) for identity, path and lock restrictions.

A committed disk decision finishes target publication before source removal and
is replayed before client handling after restart. This protects one MOVE;
it does not provide crash-atomic durability for an entire multi-command EXEC.
SIGKILL recovery tests verify process interruption, not physical power loss.

## Replication and remaining work

The private PKV4 stream carries actual frontend moves into supported memory
replicas. Versions 1 through 3 cannot represent MOVE and are disconnected before
any unsupported group prefix is sent. Page-only groups keep their older wire
encoding. Active expiry after MOVE publishes the target DB's deletion.

Disk receivers reject mixed MOVE groups before publishing a prefix: durable
multi-command replay still needs a group decision/recovery design. Initial
synchronization, reconnect catch-up and Redis PSYNC remain unfinished. A live
stream alone cannot initialize a replica from an existing dataset. These are
material limits for production replication, not completed Redis compatibility.
Private stream caps remain 65525-byte keys, 16384-byte roots, 64 MiB groups and
65536 records. This work establishes no overall performance comparison against
an unmodified official Redis server. Execution evidence is macOS only.

## Validation

`move_commands` compares argument errors, metadata, ACL keys, cluster routing,
five paged types, binary keys, TTL, collisions, WATCH, blocked readers and EXEC
with the local reference binary. Native configurations also test default-auto
restart and explicit-off behavior. That reference binary is locally modified;
it is not an official performance baseline.

`move_key` covers legacy normalization and native payload retention.
`move_live_stream` starts actual frontend primaries and an MDB replica, verifies
five types through successive moves, old-peer rejection and background expiry
without a key read on the primary. `move_online_recovery_rdb/wdb` interrupts a
real frontend transfer only after observing its pending durable decision, then
restarts and compares the complete payload and neighboring data.

Eight macOS configurations completed all-target builds and 316 selected CTests:
MDB 37/37 in each mode, oneTBB 39/39, RocksDB 41/41 and WiredTiger 41/41.
Each backend ran C++11 Release and C++17 ASan/UBSan configurations. Native online
recovery checks used a 4 MiB String and preserved neighboring data. The final
[validation report](benchmarks/public-move-validation-2026-11-02.json) records
exact selections, source epochs, dependency hashes and retained artifacts.
This is a targeted matrix, not the full suite. Earlier library and transport
reports remain unchanged.
