# CLIENT KILL

CLIENT KILL now closes matching public Redis connections across worker threads.
The old `CLIENT KILL <addr>` form returns OK or `ERR No such client`. The option
form returns the number of connections that first transition from open to
closing. Its filters are ANDed; repeated options use their last value.

Supported options are ID, TYPE, USER, ADDR, LADDR, MAXAGE and SKIPME. ID accepts a
canonical positive decimal integer up to the platform LONG_MAX. MAXAGE accepts
a positive signed-64-bit integer in seconds. SKIPME defaults to YES in the
option form; the legacy address form can close its own connection. Option/type
names are case insensitive, while usernames and full address bytes are exact.

Named users are now provided by [runtime ACLs](ACL_PERMISSIONS.md). All public frontend connections have type
normal, including frontend connections to a configured replica. Valid master,
slave/replica and pubsub filters currently match no public connection. The
companion replication protocol is outside this registry; full Redis replication,
Pub/Sub remains unfinished. USER validates the current ACL identity and
matches its generation, including the default user.

## Execution and replies

All options are validated before looking up targets or changing stop states.
The command prepares its reply storage before the first shutdown. Allocation
failure in parsing, target capture or this preparation issues no target
shutdown request. Later response serialization or socket writes may
still fail after effects, as with other commands; this is not rollback support.

An ID filter uses one registry bucket lookup. Other filters capture registry
membership, release bucket locks, filter immutable records and request shutdown.
General filtering now uses unordered capture: O(N) CPU work and O(N)
temporary references. See [snapshot optimization and safety](CLIENT_SNAPSHOTS.md).
Shutdown and retirement share each record's descriptor mutex. The connection
owner retires its record before closing the descriptor, so stale snapshots
cannot shut down a replacement connection. No caller closes another handler's
socket directly. The request does not join the owning handler. Concurrent
killers count the open-to-closing transition once.

Address strings are captured at connection creation: IPv4 `ip:port`, IPv6
`[ip]:port`, and the local listener's Unix socket `path:0`. Failed endpoint
queries use `?:0`. Creation time uses Unix seconds. MAXAGE uses the command's
existing time snapshot, including the frozen snapshot inside EXEC. No address
formatting, metadata locks or additional counters enter normal key commands.

Standalone CLIENT commands bypass the KV execution gate. An administrator can
request shutdown while another worker owns exclusive KV admission. The target
checks its atomic stop state before each subsequent parsed pipeline command;
an already executing command can finish and its effects are not rolled back.
Blocking-command cancellation needs additional integration when blocking
commands are implemented.

MULTI queues CLIENT KILL normally. DISCARD causes no shutdown. EXEC executes it
at its queued position, and runtime option errors preserve other queued command
execution. When the target is the caller, shutdown is deferred until its reply
is written. Subsequent pipeline roots are skipped. A self-kill inside EXEC lets
remaining children execute and sends the complete EXEC response before closing.

## Verified reference difference

The local modified Redis 8.6.2 reference suppresses replies appended after
CLIENT_CLOSE_AFTER_REPLY is set. For an EXEC containing self-kill followed by
SET, it commits SET but sends `*2\r\n:1\r\n` and closes: the second declared
array element is missing. The source guard is in networking.c's
`_addReplyToBufferOrList`. PKV sends `*2\r\n:1\r\n+OK\r\n` and closes. The
fixture preserves this reference evidence separately while requiring complete
framing from PKV; it does not reproduce the malformed response.

PKV compares complete option/address bytes, including embedded NUL. Some local
reference string comparisons stop at NUL. This avoids an address prefix with
trailing binary data selecting a different connection than its full bytes
identify. Error text sanitizes CR/LF to preserve response framing.

## Verification

`client_kill_commands` compares 32 common parameter/error cases and independently
checks address/local-address/type/user/age filters, repeated options, self and
other kills, pipeline suppression, EXEC/DISCARD/error timing, concurrent killers,
authentication, RESP3, real redis-cli and cluster routing. The native core fixture
holds exclusive KV admission in one thread while another executes CLIENT KILL;
it also injects allocation failures before shutdown in legacy and option forms.
The registry fixture checks IPv4 and IPv6 endpoint symmetry and retains its
500 descriptor-reuse cases and 300 stop/retire/reuse races.

Build logs, final source hashes, full CTest results and the standalone registry
ThreadSanitizer result are retained in the
[validation report](benchmarks/client-kill-validation-2026-10-31.json).
CLIENT INFO/LIST have since been added with documented allocation semantics;
see [their scope](CLIENT_INFO.md). This change does not complete the overall Redis-server goal.


## Full validation and local GET observation

All eight configurations passed 726/726 CTests: memory and C++11 memory 81 each,
oneTBB and C++11 oneTBB 90 each, RocksDB Debug/Release 96 each, and WiredTiger
Debug/C++11 96 each. WT suites ran separately. The standalone registry TSan test
also passed; it does not instrument the entire ACL service. Final source,
binary and log hashes are in the validation report above.

A separate checked GET comparison uses the retained pre-KILL registry binary
and the current C++11 Release binary with identical compiler flags, two server
threads, four clients, 64-byte values, 256-command pipelines and active expiry
disabled. Three rounds rotate baseline RESP2/current RESP2/current RESP3 order.
Every one of the 37,748,736 measured response frames matched expected bytes.
Median rates were 1,496,906/s for baseline RESP2,
1,480,419/s for current RESP2 (-1.10%) and
1,487,171/s for current RESP3 (-0.65%). This
run observes a small decrease and does not establish its cause or statistical
significance. GET does not exercise KILL filtering, connection churn or shutdown
latency; this is not a stock-Redis performance comparison or a general speedup
claim. See the [per-sample data](benchmarks/client-kill-get-2026-10-31.json).

```sh
python3 tools/benchmark_resp3_get.py \
  --baseline /tmp/pkv-registry-measured-c11-pkv \
  --pkv /tmp/pkv-kill-measured-c11-pkv \
  --baseline-flags /tmp/pkv-registry-c11-flags.make \
  --new-flags /tmp/pkv-kill-c11-flags.make \
  --batches 4096 \
  --output doc/benchmarks/client-kill-get-2026-10-31.json
```
