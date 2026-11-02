# macOS fiber event initialization

A public-MOVE regression run exposed an intermittent startup crash in the
C++11 service under ACL supervisor mode. The readiness PING was reset. A later
run passed, but a new dump at the failure timestamp established an actual
`EXC_BAD_ACCESS`, rather than a harmless timing observation.

LLDB located the failed call in ACL's `kqueue_wait`: the indirect branch used
a null function pointer. Other threads were waiting for `hook_api` to finish.
The linked ACL source assigns `__sys_kqueue` before `__sys_kevent`, while event
creation enters `pthread_once` only if `__sys_kqueue` is null. A concurrent
scheduler can therefore skip initialization during that partially published
state and call the still-null `__sys_kevent`.

The test-only interposer delays `dlsym("kevent")` for 300 ms. It does no file or
socket I/O in that window. The old binary fails during supervisor startup, with
one thread in the delayed lookup and another at address zero in `kqueue_wait`.
The raw dumps are retained outside the repository; only metadata and symbolic
backtraces belong in validation artifacts.

The server boundary now initializes one scheduler on a joined temporary
thread before starting ACL service workers. A one-millisecond fiber timer forces
event creation and a kernel event pass. Thread completion precedes subsequent
worker creation, so the process-wide event hook pointers are fully published
before concurrent use. The temporary scheduler's thread-local state is released
on thread exit. The installed ACL library and ordinary hot paths are unchanged.
This is a macOS-specific initialization workaround for the linked dependency.

The signal waiter starts before this initialization. Startup cancellation keeps
its existing immediate-exit behavior and does not depend on scheduler progress.
An interrupted native initialization can still require recovery on reopening.
Linux and Windows behavior is outside this macOS change's execution evidence.

`fiber_hook_startup` must load the interposer and complete the four supervisor
combinations of active expiry and replica receiver state. It reuses callback
shutdown checks, including five paged types in two DBs, retained replica owners,
normal callback exit and native restart. WT configurations additionally check
clean reopening when the clean-exit checker is available. Ordinary signal,
startup cancellation and callback tests remain required alongside this probe.

The temporary preview passed the four fault-injected memory combinations.
The integrated C++11 binary passed the injected test, ordinary callback exits,
startup cancellation, public MOVE and live MOVE publication (five CTests).
The final eight-configuration matrix completed all-target builds and 316
selected CTests. Each configuration ran `fiber_hook_startup`, giving 32 injected
supervisor combinations in that matrix. The [validation report](
benchmarks/public-move-validation-2026-11-02.json) retains the old-binary control,
symbolic backtraces, source epochs and final configuration-specific results.
No new corresponding service dump was observed after the fix.
