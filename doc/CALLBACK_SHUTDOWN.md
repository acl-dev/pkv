# ACL callback shutdown

ACL's idle limit, use limit and supervisor-disconnection monitor all call
`master_service::proc_on_exit`. The callback now enters the same terminal
shutdown operation as SIGTERM/SIGINT. Previously it only released the service's
DB references and returned to ACL, which stopped workers and called C++ `exit`.
Other owners, including a blocked replication receiver, could retain databases.

The common operation stops expiry before acquiring exclusive command
admission, closes all opened native stores, and retains exclusive admission
through `_exit`. It does not rely on DB reference counts or static destruction
to close the stores. Callback exceptions produce status one. Shutdown logs
distinguish a service callback from a received signal.

No command dispatch, wire format, codec, per-key record, page layout or memory
container changes are involved. Memory collections remain in their native
containers until process reclamation; no whole-collection serialization is
introduced.

## Before-fix evidence

Four isolated WiredTiger C++11 cases exercised idle/use limits, each with and
without a replica receiver blocked on an owned upstream socket. All seeded
64 KiB string data in logical DBs 0 and 7. The unchanged shutdown660 executable
produced one SIGSEGV during idle exit. Its matching retained executable and
dSYM resolve frames in `db::~db`, `cluster_manager::~cluster_manager` and ACL's
server exit path. This locates the failure in destruction, without establishing
the exact lifetime violation from those frames alone.

Both replica cases returned zero but left both stores requiring recovery.
The new native checker rejects these old stores. WiredTiger's open operation
with `log=(enabled=true,recover=error)` reports the recovery requirement and
returns `WT_PANIC`: its error cleanup promotes a recovery failure to panic.
This is not evidence of a header/library error-code mismatch.

The initial eight-case callback fixture also fails against the unchanged
executable, which does not perform or log the explicit storage shutdown. The
final fixture adds four supervisor cases. The original four-case
probe and native-check rejection separately establish the behavior rather than
relying solely on the new log marker.

## Regression scope

`server_callback_exit` covers idle limit, use limit and, on POSIX, the actual
daemon supervisor status-channel disconnect. Each runs with expiry off/on and
with/without the retained replica receiver: twelve cases per POSIX build.
The fixture confirms the receiver sent negotiation data before keeping its
upstream connection open. Each case writes and checks 64 KiB String, Hash,
List, Set and ZSet values in logical DBs 0 and 7.

Supervisor cases execute PKV's daemon branch with a duplex status socket on
ACL's fd 5, receive the startup notification and close the parent endpoint.
They require ACL's disconnect log and keep a client connected through exit.
These exercise the real callback branch without running an external ACL master;
they do not validate a complete supervisor deployment or restart policy.

All cases require status zero and the native-close completion marker. Timeout
cleanup kills the test process and leaves the test failed. Disk cases restart
the same storage and compare all acknowledged typed values. WiredTiger cases
also open both homes with recovery forbidden before restarting PKV, so an
ordinary recovery on restart cannot hide a missing native close.

## Validation

All targets built in eight configurations and 514 selected CTests passed.
This is not the complete CTest suite.

| Backend | Debug C++17 ASan/UBSan | Release |
| --- | --- | --- |
| MDB | 56/56 | 56/56, C++11 |
| oneTBB | 64/64 | 64/64, C++11 |
| RocksDB | 68/68 | 68/68, C++17 |
| WiredTiger | 69/69 | 69/69, C++11 |

After that matrix, a test-registration guard was tightened so combined backend
builds do not pass a WT checker to a test selecting TBB or RocksDB. All eight
configurations were regenerated and rebuilt: binaries, compile flags and
selected test definitions remained byte-identical. Two combined configurations
also verified the generated backend/checker selection; their services were not
built or executed. Both source manifests and archives are retained, so the
matrix input and final registration-only adjustment are distinguished.

The [validation report](benchmarks/callback-shutdown-validation-2026-11-02.json)
records exact selections and evidence. Matching executables and UUID-checked
dSYM bundles, source archives, before-source copies and logs are retained under
`build/validation/callback-exit-2026-11-02`. Optimized Release symbols may lack
source lines; symbol-generation warnings are retained. Raw dumps remain in
Crashpad. Runtime dependency hashes link to matching retained parent copies.

## Remaining work

Startup cancellation, Windows/Linux execution, the public Redis SHUTDOWN
command, memory persistence and replication queue drain still require separate
work. This change does not prove power-loss durability, full Redis compatibility
or a performance advantage over an unmodified official Redis build.

See [normal-signal shutdown](PROCESS_SHUTDOWN.md) and
[stages and estimates](ROADMAP_STATUS.md).
