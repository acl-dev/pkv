# HELLO and typed RESP3 replies

Connections start in RESP2. `HELLO 2`, `HELLO 3` and argument-free `HELLO`
select/report the protocol and return connection metadata. The map is a flat
array in RESP2 and a native map in RESP3. A successful request can set a client
name with SETNAME; repeated options use the last supplied value, with every
name validated before any change. Invalid versions/options/names leave the
protocol and previous name unchanged. RESET restores RESP2 along with the
existing database, name, WATCH and queued-transaction cleanup; ID and library
attributes remain unchanged.

The compatibility reference is local modified Redis 8.6.2, including its
source reply builders. PKV reports `server=pkv`, development `version=0.0.0`,
its own stable connection ID, configured mode/role and an empty module list.
It does not claim to be Redis 8.6.2 or advertise the reference's built-in
vectorset module. Role reflects the configured live RPC receiver setting;
complete cluster failover/role management is still unfinished.

## Wire types and connection state

The existing reply tree now carries a small response-kind annotation, separate
from parsed request types and storage values. Default RESP2 arrays/bulk strings
remain unchanged. RESP3 emits unified nulls, map/set headers and native doubles.
Ordered field/value and member/score pairs become nested arrays where Redis
requires them. ZPOP without COUNT remains a flat pair, while counted pops use
nested pairs. HGETALL, LCS IDX, supported CONFIG GET and basic COMMAND DOCS
produce maps. Set membership/algebra and counted SPOP produce sets; counted
SRANDMEMBER remains an array. Hash random field/value pairs remain ordered
arrays. Float-increment commands keep their Redis bulk-string replies.

COMMAND INFO annotations include flag/category/tip/key-spec sets, nested
key-spec/search maps and recursive supported subcommands. The existing metadata
filter still advertises only implemented commands; optional key-spec notes are
omitted as before. DOCS remains the documented summary/group subset. Scan
results keep their existing array/bulk shapes, including ZSCAN score strings.
No new record encoding, database page layout, replication wire version or
external dependency is introduced.

Each command's reply records its protocol at execution time. This is necessary
because a pipeline can contain HELLO, RESET and ordinary commands, and EXEC can
contain several HELLO commands. A later protocol change cannot re-encode an
earlier null/map/double reply. Nested EXEC replies keep their individual protocol
choices. Returning cached objects clears both kind and protocol annotations.
The two byte-sized annotations occupy existing alignment padding on the tested
64-bit layout; no per-reply protocol-state heap object is allocated.

The request parser continues accepting Redis command bulk arrays and inline
requests. This adds server response support for the types used by implemented
commands, not arbitrary RESP3 value parsing. Pub/Sub pushes, client tracking,
streams, scripting and their future response shapes are not implemented by this
change. Existing parser depth/frame limits and command/storage concurrency are
retained.

## HELLO AUTH scope

[Default-user authentication](AUTH.md) now supports startup requirepass/masterauth
and AUTH. HELLO AUTH validates the configured default user's password and leaves
protocol/name/login state unchanged on failure. Open services retain their
unrestricted default/no-password behavior. Per-user ACL authorization and TLS
are still unfinished. HELLO contributes no keys to cluster/source EXEC routing.

## Verification

C++ response cases run in both C++11/C++17 and both cache implementations. They
check RESP2 fallbacks, RESP3 null/map/set/double encoding, binary lengths,
ordered pairs, mixed-version EXEC children, odd-map rejection, invalid protocol
rejection and repeated cache reuse. Existing allocation-failure cases remain
enabled. Client-state tests cover the default protocol, connection isolation and
RESET, alongside the exhaustive 256-byte metadata validation.

TCP tests compare 236 typed command replies with isolated local Redis, including
Hash/Set/ZSet operations, LCS, floating values/infinities, all advertised leaf
COMMAND INFO schemas and key flags. Maps/sets are compared without order;
ordinary arrays and ordered pair arrays retain exact order. Optional key-spec
notes are excluded consistently. Separate assertions cover handshake identity,
version/error boundaries, option validation, retained state after failure,
strict NUL option rejection, repeated protocol changes, mixed pipelines/EXEC,
RESP3 WATCH abort nulls, cluster no-slot negotiation and configured receiver
role. The actual redis-cli `-3` path runs against both PKV and the reference.

An earlier test assumption that the reference had no loaded modules was wrong:
its HELLO contains the built-in vectorset module. The corrected checks preserve
module type/identity checks and require PKV's own empty module list. Another
fixture correction accounts for redis-cli's raw map display (`field value` on
one line). Retained diagnostic logs distinguish these fixture assumptions from
protocol command failures.

A separate pipelined hot-key GET comparison uses retained pre-change and new
C++11 Release PKV binaries, identical compiler flags, the same two-thread memory
configuration and checked response bytes. It compares PKV revisions/protocols,
not stock Redis. Mixed workloads, other backends, tail latency, long-running
races and end-to-end throughput superiority require further measurements.


The final per-configuration suites passed 678/678 CTests: memory/C++11 memory
75 each, oneTBB/C++11 oneTBB 84 each, RocksDB Debug/Release 90 each and WiredTiger
Debug/C++11 Release 90 each. Every configuration passed 236 typed comparisons
and redis-cli -3 against both servers. The early RocksDB Debug allocation-test
failure has no established cause; original-source relinking, 20 repetitions
and an isolated full rerun passed without weakened checks. See
[validation metadata and retained anomaly](
benchmarks/resp3-validation-2026-10-31.json) and
[scoped GET performance comparison](RESP3_PERFORMANCE.md).
