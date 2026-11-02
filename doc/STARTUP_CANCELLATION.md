# Startup cancellation and serving handoff

POSIX SIGTERM/SIGINT now have a waiting thread before ACL, configuration and
native storage initialization. The service publishes its normal shutdown
callback only after initialization completes. A mutex orders this publication
against the waiting thread's choice of startup cancellation or serving stop.

Before publication, no application commands have been accepted. Cancellation
uses `_exit(0)` and lets the OS reclaim partially initialized objects. It does
not attempt cross-thread C++ destruction or native connection closure during
an in-progress open/recovery operation. Native recovery may be required on the
next start. This is a startup interruption, not a claim of clean native closure.

After publication, the existing shutdown callback drains command admission,
stops expiry and explicitly closes native stores before exiting. Callback
exceptions and callbacks that unexpectedly return exit with status one.
Registration rejects missing callbacks, publication before starting the waiter,
repeated starts and attempts to replace a published callback.

The waiter state, mutex, callback and thread object have process lifetime.
They remain valid if a startup error runs static destructors while the waiter
is still blocked. Startup cancellation and waiter failures perform no log I/O:
a full stderr pipe must not prevent termination. The enabled service callback
retains its own logging behavior. No DB or scheduler work runs in a signal handler.
Neither branch adds fields or synchronization to keys, pages or ordinary
command execution; the native oneTBB containers and paged formats are retained.

## Reproduction

The unchanged callback662 C++11 memory executable blocked while reading an
owned ACL FIFO. Both SIGTERM and SIGINT remained pending beyond one second.
Only releasing the FIFO let initialization finish, create the old late waiter
and process the stop. Both then exited zero. The probe records the actual
executable hash, signal, timing and exit status; it did not force-kill a child.

## Regression design

`startup_signal` uses real server processes with an owned main-config FIFO or
ACL FIFO. It confirms the reader has opened the FIFO, retains the writer and
requires each shutdown signal to exit zero within two seconds without releasing
the blocked input. Native storage must not have been created in those cases.

The linked `startup_signal_helper` exercises the same signal API and handoff.
It checks cancellation before callback publication, successful enabled
callbacks, throwing callbacks and erroneously returning callbacks. For native
engines it also holds a real opened DB before publication. The fixture seeds
five 64 KiB paged types in DBs 0 and 7 through the server before interruption,
then reopens the server and compares acknowledged values.

Each configuration/FIFO case and each basic handoff/error mode also runs with
stderr connected to an owned, completely filled pipe that the parent never
drains before exit. Native open/recovery cases retain captured stderr for
sanitizer diagnostics.
The initial implementation's startup message blocked on such a pipe: a probe
filled 65536 bytes, observed SIGTERM still waiting after one second, then
drained the pipe and observed exit zero. The corrected startup branch performs
no writes. The helper's enabled callback exits seven to distinguish it from
startup cancellation's zero; throwing/returning callbacks must exit one.

Separate helper cases open two physical roots and two logical databases, then
run real MOVE recovery. They pause after a page copy, target-root publication
or source-root deletion. Each signal cancels the helper with the durable
decision still present. The actual server must then finish recovery before
PONG, retain both neighboring values, remove the source and expose the correct
target value. These are native recovery component cutpoints, not injected
hooks inside the production server or evidence of a public MOVE command.

The existing disk recovery and service startup fixtures provide additional
failure/corruption/topology coverage. Tests distinguish startup cancellation
from serving shutdown, rather than treating recovery on a later open as proof
that a partially opened connection was cleanly closed.

## Validation

The final frozen 664-file source built all targets in eight configurations and
passed 540 selected CTests. This is not the complete CTest suite.

| Backend | Debug C++17 ASan/UBSan | Release |
| --- | --- | --- |
| MDB | 58/58 | 58/58, C++11 |
| oneTBB | 67/67 | 67/67, C++11 |
| RocksDB | 72/72 | 72/72, C++17 |
| WiredTiger | 73/73 | 73/73, C++11 |

The startup fixture contains 24 scenarios per MDB/TBB configuration and 32 per
native configuration, totaling 224 across the matrix. The existing startup
fixture now requires configuration rejection to return one and normal stops to
return zero without forced kills. An earlier matrix was intentionally stopped
after discovering stderr backpressure; its six completed configurations and
incomplete WT run are retained separately and excluded from the final 540.

Initial and corrected focus runs are also separate from the final matrix.
The stderr-pipe reproduction retains the old helper with its executable hash;
the new negative fixture rejects that helper.

The [validation report](benchmarks/startup-cancellation-validation-2026-11-02.json)
records source versions, exact selections, flags, binaries and evidence.
Final servers and startup helpers have UUID-matched dSYM bundles. Executables,
source archives, before-source copies and logs are retained in
`build/validation/startup-cancellation-2026-11-02`.
Optimized/LTO objects may lack source lines; symbol-generation warnings are
retained. Raw dumps remain in Crashpad and are not copied into the repository.

## Limits and remaining work

These changes target POSIX lifecycle handling. Windows retains its existing
signal path; Linux/Windows execution requires separate validation. Native
power-loss durability, complete Redis SHUTDOWN semantics, memory persistence,
unfinished replication groups and outgoing replication queue drain remain
separate work. No throughput measurement is established here.

See [normal-signal shutdown](PROCESS_SHUTDOWN.md),
[callback shutdown](CALLBACK_SHUTDOWN.md) and
[stages and estimates](ROADMAP_STATUS.md).
