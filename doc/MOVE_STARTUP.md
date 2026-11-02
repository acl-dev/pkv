# Native MOVE startup recovery

The server can now recover an existing internal MOVE decision before worker
threads, expiry, the replication receiver or the private RPC service start.
This completes the startup boundary for [native redo](NATIVE_MOVE_REDO.md).
The [public MOVE command](MOVE.md) now uses the same journal and coordinator
for online native transfers.

## Configuration and storage identity

`move_journal_path` defaults to `auto`. RocksDB (`rdb`) and WiredTiger (`wdb`)
select `.pkv-move.decision` in the first canonical configured storage root.
Memory backends treat `auto` as disabled. `off` keeps unbound native pools
disabled; bound pools reject disabling the journal. An empty configuration
value retains ACL's default `auto`; request `off` explicitly. The lower-level
domain API also accepts an empty string as disabled.
An explicit journal requires native storage and an absolute file path whose
parent already exists.
For example, after creating `/srv/pkv/control`:

```
dbtype = rdb
dbpath = /srv/pkv/data-a;/srv/pkv/data-b
databases = 16
move_journal_path = /srv/pkv/control/move-decision
```

Use a stable, dedicated control directory outside managed engine directories.
The decision may not overlap a storage root, `rdb`, `wdb`, `logical-db-*`,
`.pkv-move.domain` or `.pkv-move.domain.lock` inside a configured root.
The file itself need not exist. Its sibling `.lock` file remains after recovery.
Memory backends reject an explicit journal path, since they cannot recover a
durable source after restart.

Enabling this option persistently binds every root using `.pkv-move.domain`.
The versioned, checksummed identity includes the normalized backend, logical DB
count, ordered canonical roots and canonical decision path. Startup uses those
same canonical roots for DB creation and logical DB discovery. Duplicate roots
are rejected. Canonical roots containing the native path-list separators
`;`, `,`, space, tab, CR or LF are rejected when this option is enabled.
Aliases resolving to the same canonical paths preserve identity.

A bound pool rejects a different root order, logical DB count, backend or journal
path. It also rejects disabling this option, even after its decision was cleared.
An empty journal alone cannot detect a forgotten old path; the persistent root
markers provide that check. A partially installed set of markers can resume with
the exact same identity. A mismatching or damaged marker is never overwritten.
Marker reads and the encoded identity are bounded to 64 KiB.

There is no automatic migration, unbinding or old-binary downgrade protocol.
Keep this configuration stable and preserve all control files with the storage.
Manual removal or copying of control files bypasses the binding contract.
POSIX advisory locks coordinate cooperating processes; they do not prevent an
administrator or an older binary from modifying files directly. Directory
renames, remounts or replacement of storage under an unchanged pathname require
external administration. Identity describes canonical configuration paths,
not a hardware volume identifier or an adversarial filesystem proof.

## Startup sequence

1. Prepare the domain before opening native storage: resolve paths, check all
   surviving markers, acquire root locks in sorted order, and open/validate the
   decision journal with the exact identity. No new binding marker is written.
2. Construct the observer object without starting its threads. Open DB 0 across
   all configured physical stores and construct its lazy logical DB pool.
3. Bind the domain after native engine ownership succeeds. Validate all markers
   again; synchronize existing markers and their directories. Publish missing
   markers through file sync, close, rename and directory sync. Expose the
   journal only after all markers succeed.
4. Run native redo with exclusive admission and retained source/target locks.
   Target pages and root become durable before source removal; source removal
   becomes durable before the decision is cleared. Preserve absolute expiry.
5. Initialize the manager and start ordinary worker, expiry and RPC activity.

The two phases avoid publishing markers when first activation loses the native
DB lock to an already running default server. That existing server remains
usable. Once bound, a competing enabled server using the same configured
roots fails on root ownership before opening those stores. The master service owns the root control locks and journal; `close_db`
deliberately retains them. Member order releases its local DB references before
control ownership during teardown. Native engine locks protect DB references
retained elsewhere until process exit. Serving-time signal and callback shutdown now close native stores under
exclusive admission; see [stage status](ROADMAP_STATUS.md). Startup cancellation
can still require recovery and does not promise a normal native close.

Startup errors fail closed. A damaged decision, identity mismatch, native open
failure, marker IO error or redo error prevents request handling. Redo failures
retain the decision for retry. A failed bind may leave correctly published
markers, which the same configuration can resume. The framework can bind its
client listening socket before `proc_on_init`; a bound socket is not evidence
that requests can run. The recovery boundary precedes client handling and RPC
worker creation, rather than promising that no socket was ever bound.

Disabled native startup only inspects root markers and rejects any existing
binding. It does not create control directories, canonicalize paths, acquire
control locks or write markers. Relative paths and ordinary unbound aliases
retain the existing native open behavior. This change adds no per-key fields,
collection serialization, page layout or ordinary write/WAL policy changes.
It makes no new throughput or Redis performance comparison claim.

## Startup baseline validation scope

`move_domain` checks ownership, exact identity, every-byte marker corruption,
partial installation, symlink rejection, new-marker IO failures, existing-marker
read/seal failures, short reads and EINTR. `move_startup_mdb/tbb` checks rejection
of durable journals and ordinary disabled startup on volatile backends.

`move_startup_rdb/wdb` launches only owned processes on temporary roots and
allocated test ports. A native helper prepares a genuine committed decision
using two physical roots, a binary owner, two bounded pages and a neighboring
owner. Five states cover unapplied decisions, interruption after a page commit,
target publication, source removal, and an expired source. The first successful
PING follows journal completion; GET/SELECT verify visibility, unrelated data,
ordinary writes and a second restart. Offline checks verify source removal.

Seven rejection cases cover disabling, DB count, root order, partial topology,
damaged decisions, damaged markers and a changed journal path. They preserve
control bytes, avoid creating an unintended native DB and recover after the
correct configuration is restored. Two process contention cases check an
existing default owner and an existing enabled owner, including continued SET
and GET on the original process. Test helpers disable core dumps and terminate
only their own children. They never connect to an existing user Redis server.

These are startup and process-interruption checks. They do not simulate power
loss, device faults, a storage migration, online MOVE or grouped replication.
This revision passed **362 selected CTests** across eight C++11 Release
and C++17 ASan/UBSan configurations: MDB 37/37, oneTBB 45/45, RocksDB
49/49 and WiredTiger 50/50. This is a targeted regression selection, not a
full CTest matrix. All 607 code/test/build fingerprints match the frozen
tested state; 597 parent files remain byte-identical. The three changed
parent files are the master service source/header and test registration;
seven domain/startup files were added.

The recorded matrix and exact source/configuration/log fingerprints are in
[startup validation](benchmarks/startup-validation-2026-11-01.json).

The public command changes the default to `auto`; disabled-mode fixtures now
request `off` explicitly. Public online and default-auto validation is described
in [MOVE](MOVE.md), separately from the historical startup baseline above.
