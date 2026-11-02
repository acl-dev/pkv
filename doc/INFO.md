# INFO and statistics

INFO accepts multiple case-insensitive section names, DEFAULT, ALL and
EVERYTHING. Repeated sections appear once; unknown sections produce no fields.
RESP2 returns a bulk string and RESP3 returns a `txt` verbatim string. Values
use CRLF framing. COMMAND metadata and ACL categories include INFO and
CONFIG RESETSTAT.

## Sources and scope

| Section | Implemented information |
| --- | --- |
| Server | OS, architecture, actual PID/port, random run ID, uptime, compiler |
| Clients | Connected clients and PKV client-buffer/watch diagnostics |
| Memory | Actual allocator bytes when available and process resident bytes |
| Persistence | Native storage engine and absence of Redis RDB/AOF activity |
| Stats | Connections, executed commands, network bytes, errors, ACL denials |
| Replication | Configured role and explicit private RPC protocol label |
| CPU | Process and reaped-child CPU time from the operating system |
| Commandstats | Calls, elapsed microseconds, rejected calls and failed calls |
| Errorstats | Actual error reply counts grouped by error prefix |
| Cluster | Actual cluster-mode setting |
| Keyspace | Logical roots, expiring roots and sampled remaining TTL per DB |

The development version remains 0.0.0, consistent with HELLO. A Redis version
claim would imply compatibility that is still under development. Modules has
an empty section because there is no module loader. Unsupported instantaneous
rates, peaks, keyspace hit rates and replication offsets are omitted.

`used_memory` measures live allocator bytes: ASan's allocator when enabled,
jemalloc's allocated statistic when configured, all malloc zones on macOS, or
glibc mallinfo2 on supported Linux versions. It is omitted if unavailable.
`used_memory_rss` is the OS resident size and includes more than stored values.
These figures describe the PKV process; they do not use Redis's allocator
accounting conventions. Native disk durability remains separate from Redis
RDB and AOF. The configured replica role does not imply PSYNC support or
complete Redis replication.

## Accounting and reset

Commands queued by MULTI are counted when EXEC executes them. Admission
failures increment rejected_calls; execution errors increment failed_calls.
An ordinary child execution error is counted once in errorstats and on that
child, without marking EXEC failed. A queued command denied by a changed ACL
marks EXEC failed, matching the tested Redis reference behavior. Unknown
commands contribute error replies without inventing commandstats entries.
INFO's own call is published after preparing its response.

CONFIG RESETSTAT stages a complete baseline and publishes it only after all
allocation succeeds. It resets command rows, received-connection totals,
network byte totals and error totals/prefix counts. The reset command itself
then counts as one successful execution. Current client records, run ID,
uptime, data, ACL audit entries and cumulative ACL denial totals survive.
ACL denials are counted even when audit retention is disabled.

Received bytes are counted at input ingestion and sent bytes after successful
response writes. A parsed pipeline resets already-received input before its
responses are flushed. Protocol bytes and error replies are measured on the
Redis frontend; companion RPC traffic is outside these counters. If error
prefix allocation fails, the total still increases and
`pkv_errorstats_untracked` exposes the difference.

## Concurrency and keyspace costs

Each OS worker owns its counters and publishes atomic snapshots. Normal KV
execution adds no global statistics lock or shared atomic increment. Command
indexes are cached in the service dispatch table. INFO and RESETSTAT acquire
exclusive execution admission, coordinating with native transactions. CLIENT
commands that can run outside that admission publish under a worker-local
mutex also used by snapshots. Input and rejection totals remain observational
while other connections are active; they are not a simultaneous global cut.
Elapsed command time uses a monotonic clock and includes admitted execution.

Key counts include logical roots, including lazily expired roots awaiting
cleanup, and exclude private pages. The TTL index gives exact expiring-root
counts. avg_ttl estimates remaining time from at most 16 owners per each of
64 buckets, with expired samples clamped to zero and overflow-safe averaging.
It is not Redis's identical sampling algorithm.

INFO includes opened DBs and existing canonical logical-db-N directories
discovered at startup for RocksDB/WiredTiger. It opens those stored DBs lazily
when reporting, without creating every configured empty database. Unavailable
stored DBs return an ERR reply while keeping the connection usable. A first
report can rebuild a dirty expiry index
and scan native roots; later reports use the index. INFO is administrative
work and can pause admitted KV commands during that work. No serialized
value format or bounded page layout changes in this increment.

## Validation

Socket tests compare stable command/error/reset/ACL observations with an
isolated local Redis 8.6.2 reference, plus sections, RESP3, EXEC visibility,
network bytes, concurrent totals and disk restart discovery. A corrupted
nonzero DB directory must return an error and leave PING usable. Native tests
exercise reset allocation failures, error-prefix allocation failure, 80,000
concurrent updates with a snapshot reader, TTL overflow and lazy DB selection.
The reference is a modified local build, not evidence for a stock Redis speed
claim. See the machine-readable validation and GET benchmark reports in
`doc/benchmarks/` for measured scope and limitations.

The final revision passed 854/854 CTests across eight macOS ARM64 builds:
memory/C++11 memory 97 each, oneTBB/C++11 oneTBB 106 each, and RocksDB
Debug/Release plus WiredTiger Debug/C++11 112 each. Debug builds used
ASan/UBSan; the native statistics TSan fixture passed separately. All 87
recorded source fingerprints stayed fixed through the final matrix.
See [validation evidence](benchmarks/info-validation-2026-11-01.json).

The checked GET benchmark used identical C++11 Release flags, MDB, two server
threads, four clients, a 64-byte hot key, 256-command pipelines and three
rotating rounds. It checked 37,748,736 measured requests with active expiration
disabled. Median throughput was 1,507,443 requests/s for the prior CONFIG
revision, 1,461,064 for the new RESP2 revision (-3.08%), and 1,408,429 for new
RESP3. RESP3 is compared with the old RESP2 baseline, so its difference also
includes protocol differences. This is a local workload measurement with no
statistical significance or stock Redis performance claim. Reducing the
measured hot-path overhead remains follow-up work. See
[raw benchmark results](benchmarks/info-get-2026-11-01.json).

The subsequent [dispatch optimization](COMMAND_DISPATCH.md) retains these
statistics and reports contemporaneous matched-protocol measurements for MDB
and oneTBB, including the earlier CONFIG revision as an MDB control.

Interface references: [Redis INFO](https://redis.io/docs/latest/commands/info/)
and [CONFIG RESETSTAT](https://redis.io/docs/latest/commands/config-resetstat/).
