# Command dispatch

The command registry now records the admission category for ACL, CONFIG,
INFO, CLIENT, KEYS, DBSIZE and EXEC during service construction. Ordinary
commands use the regular category. The registry stays immutable after startup.

The frontend uses these categories for preflight checks, CLIENT's independent
connection path and exclusive execution admission. It no longer compares each
ordinary command with every administrative command name. Null-handler control
commands retain the WATCH/UNWATCH/MULTI/EXEC/DISCARD/RESET/QUIT path; ordinary
root handlers skip its comparisons and temporary reply-position bookkeeping.

Command lookup uppercases ASCII lowercase bytes directly, independent of the
process locale. Registered Redis command names are ASCII. The frontend still
rejects embedded NUL names before lookup. Native catalog tests cover lowercase
and alternating-case forms of every registered root command, all high-byte
suffixes and four concurrent registry readers.

Arity and argument checks retain their previous order. ACL, CONFIG and INFO
still acquire exclusive execution admission. EXEC does so only in a current
transaction; standalone KEYS/DBSIZE do so for their valid arities. Standalone
CLIENT commands retain their independent path so a connection can stop another
connection while it owns KV admission. Queued CLIENT commands still run inside
EXEC. Statistics retain exact calls, errors and monotonic elapsed-time samples;
there is no sampling, disabled accounting or global statistics lock added.

No stored value encoding, page layout, storage engine or command inventory
changes in this increment.

## Checked GET measurements

Both runs use matched C++11 Release compiler flags within each backend, two
server threads, four clients, 64-byte values, 256-command pipelines, 4096
measured batches per worker, 16 warmup batches and three rotating rounds.
Active expiration is disabled. Every measured response byte is checked.

| Backend / protocol | Prior INFO revision, requests/s | New, requests/s | Change |
| --- | ---: | ---: | ---: |
| MDB / RESP2 | 1,468,620 | 1,519,589 | +3.47% |
| MDB / RESP3 | 1,468,875 | 1,534,117 | +4.44% |
| oneTBB / RESP2 | 1,421,779 | 1,475,191 | +3.76% |
| oneTBB / RESP3 | 1,430,873 | 1,462,168 | +2.19% |

The MDB run includes the pre-INFO CONFIG revision as a separate RESP2 control.
Its median was 1,478,219 requests/s; new RESP2 was 2.80% higher in this run.
This contemporaneous control addresses the previous INFO overhead measurement.
The MDB run checked 62,914,560 measured requests across five variants; the
oneTBB run checked 50,331,648 across four variants. Both include prior and new
RESP3 so protocol changes are not mixed into the matched comparisons above.

These are macOS ARM64 hot-key results, not mixed-workload, disk-backend or
stock Redis performance claims. Three samples per variant do not establish
statistical significance. No individual change is isolated by these combined
measurements. See [MDB raw results](
benchmarks/dispatch-get-mdb-2026-11-01.json) and [oneTBB raw results](
benchmarks/dispatch-get-tbb-2026-11-01.json).

## Final validation

The final source passed 854/854 full-matrix CTests on macOS ARM64: memory and
C++11 memory 97 each, oneTBB and C++11 oneTBB 106 each, RocksDB Debug/Release
and WiredTiger Debug/C++11 112 each. Debug configurations used ASan/UBSan.
Existing test timeouts, concurrency checks and disk synchronization settings
were retained. All 89 recorded source fingerprints stayed fixed throughout
the matrix. The measured Release binaries match the full-matrix binaries.

The retained protocol checks include 259 typed RESP3 comparisons, 13 stable
INFO observations, 31 CONFIG observations and the existing ACL, authentication,
client-control, transaction and replication suites. Modified source lines are
at most 80 columns; the longest modified function is 154 lines. See
[machine-readable validation evidence](
benchmarks/dispatch-validation-2026-11-01.json).

This increment preserves the current command inventory. Full Redis command
coverage, blocking commands, Pub/Sub, scripting, Redis RDB/AOF, full replication
and complete cluster behavior remain ongoing work.
