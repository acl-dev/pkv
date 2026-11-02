# Client metadata allocation-failure investigation

The first CLIENT INFO memory build failed the existing client_state injected
allocation-failure assertions. All three large metadata copies succeeded while
the executable's replacement operator new observed zero calls. The test was
not relaxed: it still requires allocation failures to preserve prior state.

## Retained negative evidence

The failing executable is retained at
`/tmp/pkv-info-client-state-failing`, SHA-256
`1f40b95a361e99ad8619b540d7fa819bfab20eb92783b6703d3b92e27c2e0016`.
Five ordinary runs and five DYLD_FORCE_FLAT_NAMESPACE runs reproduced the
failure; see `/tmp/pkv-info-oom-binding-probe.log`. Copying the executable
preserved the failure. Relinking the same original object and archive with
the same link arguments, changing only the output path, produced a passing
executable. `/tmp/pkv-info-oom-exact-relink.log` records that comparison.

Disassembly in `/tmp/pkv-info-oom-failed-asm.txt` and
`/tmp/pkv-info-oom-relinked-asm.txt` shows identical metadata-copy code calling
the exported libc++ string copy constructor. Separate small copy/range probes
both reached the allocation hook. Thus the observations do not prove that all
libc++ string copy constructors bypass executable hooks, and the exact
linker/runtime cause remains unresolved. Previously unretained intermittent
failures cannot be proven to share this cause.

## Current code and verification

Metadata cloning now uses an iterator-range string constructor, instantiated
in the target. Assign, HELLO and copied snapshots keep owned strings and their
existing exception guarantees. The allocation-failure fixture source and
assertions are unchanged. This is a production copy path, not a test-only hook.

`/tmp/pkv-info-oom-range-calls.log` records the local range-initialization call;
`/tmp/pkv-info-oom-range-allocation.log` shows a direct call to the executable's
operator new. Ten runs of the final memory executable and ten separate exact
relinks all pass, recorded in `/tmp/pkv-info-oom-range-repeat.log`. This gives
concrete evidence that the current path exercises allocation failure here.
It does not establish a platform-wide explanation or erase the retained
failing-binary evidence. The full build matrix and metadata TSan checks are
recorded separately in the CLIENT INFO validation report.
