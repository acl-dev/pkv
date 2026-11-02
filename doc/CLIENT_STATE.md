# Connection metadata and RESET

PKV now supports CLIENT ID, GETNAME, SETNAME, SETINFO, KILL, INFO, LIST and HELP, alongside
RESET. These commands use connection-owned state and do not operate on key
names. The reference is the local modified Redis 8.6.2 binary/source and the
[CLIENT SETINFO documentation](https://redis.io/docs/latest/commands/client-setinfo/).

CLIENT ID returns a positive process-local connection identifier. The same
connection keeps its ID across commands and RESET. IDs are assigned once using
a relaxed atomic compare/exchange, remain unique under concurrent acceptance,
and are never recycled during that process. Exhaustion is checked before
incrementing beyond the signed RESP integer range. IDs are opaque, can have
gaps and are not persistent identities across restarts or different servers.

SETNAME records a printable ASCII name. Empty input removes it; GETNAME then
returns null. SETINFO accepts LIB-NAME and LIB-VER, case insensitively, and owns
the supplied printable library attributes. Empty attributes clear them. There
is no new metadata-length cap beyond existing request/resource constraints.
Invalid values preserve prior state; allocation failure also leaves the old
attribute intact. Name/attribute replacement releases the old owned string.

RESET clears queued MULTI commands, the transaction-failure flag, all WATCH
subscriptions, the client name and the selected database (back to DB 0). It
replies with the simple string RESET. It retains the connection ID and both
library attributes, as Redis does. Stored values and TTLs are not reset.
A queued command is discarded, not executed, when RESET arrives during MULTI.
Existing queued response frames remain intact in a pipelined request.

## Validation, routing and replies

Known CLIENT subcommands have their own arity checks before transaction
queuing. Invalid subcommands or arities dirty MULTI immediately, so EXEC aborts.
Command-body errors such as a bad name or unknown SETINFO attribute are checked
when that command executes; other valid EXEC commands still run. Metadata
commands queue normally and take effect in execution order. RESET is processed
immediately rather than queued, including after an earlier queue-time error.

CLIENT/RESET contribute no keys to cluster or EXEC routing. Names containing
hash tags cannot cause CROSSSLOT or redirection. Connection-only transactions
do not need a source storage workspace; mixed transactions retain existing
native source batching for their actual key operations. Metadata is local to
the connection and is not replicated or stored in database records.

Printable bytes are validated across their complete length. PKV rejects NULs
in names/library values and NUL-suffixed option tokens rather than accepting
C-string prefixes. The local reference's validation can stop at embedded NUL;
these strictness cases are separate from common differential assertions.
Unknown-subcommand diagnostics are capped at 128 bytes like Redis. Reflected
CR/LF characters are replaced with spaces so they cannot create extra RESP
frames. Unknown attributes retain their normal error text. Following PING
checks verify reply framing after rejected requests.

COMMAND metadata includes the eight implemented CLIENT children and RESET.
Unsupported CLIENT operations are not advertised. HELP describes the supported
subset rather than listing unimplemented reference commands. The client state
object is noncopyable, preventing accidental duplication of a connection ID.
Metadata commands need no global registry lock; only connection creation
updates the ID allocator, and ordinary key commands do not update it.

## Verification and remaining scope

The C++ fixture checks connection isolation, all 256 possible input byte values,
empty clears, self-alias updates, retained ID/library attributes after RESET,
allocation-failure preservation and eight threads assigning 8000 unique IDs.
RESET releases the name even with allocation disabled. The subcommand table
also rejects token truncation at NUL.

TCP tests compare ordinary replies with the local Redis process: valid and
invalid names/attributes, arities, errors and CR/LF reflection, SELECT/RESET,
queued and failed MULTI state, WATCH clearing, command discovery and metadata.
Independent tests cover stable unique IDs over concurrent connections, strict
NUL rejection, pipelined replies surviving later RESET, and cluster key routing.
A real local redis-cli binary exercises connection naming, SETINFO and RESET.
The original revision checked library retention directly in C++; the runtime
extension now checks it through CLIENT INFO/LIST as well. The CLI integration is explicitly
skipped if that optional executable is unavailable.

This is the implemented CLIENT subset, not the entire client-management API.
CLIENT PAUSE/TRACKING and Pub/Sub remain unfinished. Named users and
permissions are now supplied by [runtime ACLs](ACL_PERMISSIONS.md).
CLIENT INFO/LIST are implemented by the runtime extension linked below. Default-user [authentication](AUTH.md) and
[HELLO/RESP3](RESP3.md) are implemented. Protected RESET clears login state and
restores RESP2 as well as existing supported session state; future
authentication/tracking state must also be reset when those features arrive.
Connections still start in RESP2. These checks do not establish complete Redis
compatibility, whole-server race freedom or throughput superiority.


The final implementation passed 648/648 full-matrix CTests across all eight
cached memory, oneTBB, RocksDB and WiredTiger configurations, with 141 common
protocol comparisons and actual redis-cli integration in each. Debug builds
use ASan/UBSan. Counts, durations, reference/CLI/source hashes and full-log
hashes are retained in [validation metadata](
benchmarks/client-state-validation-2026-10-31.json).


CLIENT KILL has since been added to the supported child table and COMMAND
metadata. See [closure semantics and limits](CLIENT_KILL.md). Earlier
validation counts describe their retained revisions.


CLIENT INFO/LIST now use synchronized metadata plus a lifetime-protected runtime
observer. See [connection diagnostics](CLIENT_INFO.md) for query semantics and
implementation-specific resource gauges. Earlier validation counts describe
retained revisions.
