# Disabled replica-read guard fast path

`replica_read_guard` keeps its enabled flag and existing actor-local depth.
Its constructor/destructor now test the flag inline. Only enabled scopes call
out-of-line `enter`/`leave`, whose depth operations are the same as before.
On normal return, primary commands skip the two external calls previously
made even when the scope was disabled. Nested disabled scopes still borrow an active
scope without changing its depth.

This does not replace fiber-local state with a thread-wide flag, add any
per-key fields, or change the protected DB/codec/native/replication paths.
Existing enabled scopes still use the current ACL fiber's actor state on both
entry and exit. The disabled primary path does not change expiry semantics.

## Compiler evidence

The same small C++11/O3/arm64 probe was compiled before and after the change.
A function containing only `replica_read_guard(false)` previously called its
constructor and destructor. It now consists of `ret`, with no calls. The
runtime-enabled probe still branches to the actor-local helpers. This proves
elimination for the isolated constant-false guard, not zero cost for an entire
Redis command or every compiler/architecture.

The source manifest still contains 631 entries. Exactly two files changed
from the retained replica-retention baseline; the other 629 remain identical.
Reversing just the declared method-placement edits reconstructs both prior
sources byte for byte. Added code lines fit 80 columns and each new function
body fits four lines, below the 200-line limit.

The real Release handler object also branches around `enter` and `leave` when
the enabled byte is zero. Its exception cleanup still has an out-of-line
destructor, so the claim above concerns normal command completion. Object
assembly/relocations and linked-symbol fingerprints are retained with the
validation evidence.

## Functional verification

Eight all-target builds completed, followed by 458 selected CTests: MDB
Debug/Release 49 each, oneTBB Debug/Release 57 each, RocksDB Debug/Release
61 each, and WiredTiger Debug/Release 62 each. Four Debug builds use the
fingerprinted ASan/UBSan fiber runtime. WiredTiger configurations ran
separately. This is the same selected test set as the preceding retention
validation, not the full CTest suite.

Existing fixtures cover nested disabled guards, exception unwinding, sibling
ACL fibers across yields, ordinary threads, primary cleanup, writable replicas,
read-only EXEC and delayed partial pages plus PERSIST. The real TCP retention
fixture uses each configured storage backend and checks no read-induced
downstream mutation. No test expectation or production DB path changed.

Exact source/build/runtime/log fingerprints and selected test names are in
[scoped validation](benchmarks/guard-validation-2026-11-01.json). Public MOVE,
its bounded source publication, standard Redis replication, Streams, scripting
and other full-Redis requirements remain incomplete.

## Scoped primary measurements

The retained retention baseline and new C++11 Release servers use identical
flags. The independent benchmark ran after every test process exited, with
four clients, two server threads, pipeline 256 and 64-byte values. There are
84 alternating samples across the two memory backends: five rounds of GET
RESP2/RESP3 and PTTL (1048576 operations per sample), plus three rounds of
overwrite (131072 operations) and churn (262144 operations). Active expiry
was disabled. Every response and final write-key count was checked.

Median throughput change relative to that retained revision:

| Backend | GET RESP2 | GET RESP3 | PTTL | Overwrite | Churn |
| --- | ---: | ---: | ---: | ---: | ---: |
| MDB | -0.35% | -1.07% | +1.17% | +2.21% | -0.22% |
| oneTBB | -0.17% | +0.40% | +0.91% | -1.76% | -0.22% |

PTTL's measured gain is about 0.9% to 1.2%. Other medians range from -1.76%
to +2.21%. These samples do not establish statistical significance or a
general throughput gain. Loopback, Python clients, a hot read key and
uncontrolled unrelated machine load limit inference. The remaining DB checks
for active replica reads are unchanged; the optimization removes disabled
scope calls, not every possible context-check cost.

Full measurements, artifact/helper fingerprints and limits are in the
[performance report](benchmarks/guard-primary-regression-2026-11-01.json).
This compares two PKV revisions, with no official Redis superiority claim.
