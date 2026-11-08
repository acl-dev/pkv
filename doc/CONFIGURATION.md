# Runtime configuration

PKV implements CONFIG GET, CONFIG SET, CONFIG HELP and CONFIG RESETSTAT.
The previous fixed
CONFIG replies are replaced by values from the running service. This is a
configuration foundation for implemented features; it does not claim Redis's
entire configuration inventory or CONFIG REWRITE. See
[INFO and statistics](INFO.md) for RESETSTAT's scope.

## Supported values

On POSIX, the explicit `pkv alone <config>` entry selects ACL standalone mode
even with redirected stdin or a parent `MASTER_SERVICE` value. It sets
`MASTER_SERVICE=ALONE` inside the server process before entering ACL. The
supervisor/daemon entry retains its existing environment and descriptor ABI.
See the [regression and validation](benchmarks/standalone-mode-2026-11-08.md).

### Startup request-object cache settings

These worker-local settings are read from the service startup configuration
(`pkv.cf` or `mdb.cf`), separately from the Redis CONFIG GET/SET inventory below.

| Startup setting | Default | Meaning |
| --- | ---: | --- |
| ocache_max | 10000 | Maximum number of returned request/reply objects retained for reuse during active processing |
| ocache_idle_max | 1024 | Retention target after worker inactivity; zero disables idle reclamation |

Each worker samples batch activity once per second. When consecutive samples
are unchanged, it releases excess returned objects, normally after about one
to two idle seconds. Borrowed objects keep their ownership. The maintenance
fiber is created only when `ocache_max` exceeds a positive `ocache_idle_max`;
it requests a 16 KiB stack. The setting bounds object count, not total buffer
bytes or database memory.

The local libc measurements showed lower logical heap allocation after
reclamation, without a corresponding RSS decline. Physical residency remains
a separate optimization and validation requirement.

The active-processing limit remains 10000: fixing it at 1024 caused substantial
deep-pipeline regressions. See the [idle-reclamation measurements](benchmarks/performance-cache-idle-2026-11-08.md)
for memory, throughput, remaining negative results and validation scope.

### Startup worker connection dispatch

On macOS, `worker_dispatch=yes` (default) distributes new TCP connections
round-robin across the workers sharing a listener. Set `worker_dispatch=no`
to retain direct processing on the accepting worker. A single worker creates
no dispatch queue. Other platforms retain their existing acceptance path.

Each remote connection gets a duplicated, close-on-exec descriptor; its stream,
parser and handler execute on the receiving scheduler. Existing connections
are not migrated. The receiver inherits `fiber_stack_size`, `fiber_share_stack`,
`fiber_buf_size` and `fiber_rw_timeout`; descriptor/allocation pressure falls
back to local processing. This balances connection counts, not the request
volumes of differently loaded clients.

Dispatch adds worker notification fibers and can increase retained memory.
The [measurement report](benchmarks/performance-worker-dispatch-2026-11-08.md)
includes the throughput improvement, CPU regressions and higher post-load RSS.
This startup option is separate from the Redis CONFIG inventory below.

### Redis CONFIG values

| Name | GET value | Runtime SET |
| --- | --- | --- |
| aclfile | Configured ACL file path | Immutable |
| acllog-max-len | Current audit retention limit | Nonnegative native long |
| appendonly | no | no is a no-op; yes fails explicitly |
| cluster-enabled | Actual cluster mode | Immutable |
| databases | Usable logical database count | Immutable |
| replica-read-only / slave-read-only | Current client-write policy | yes/no |
| requirepass | Remembered frontend configuration password | Binary safe |
| save | Empty Redis snapshot schedule | Empty is a no-op |

RocksDB/WiredTiger durability is separate from Redis RDB/AOF formats. A
nonempty save schedule or appendonly yes fails because those Redis persistence
mechanisms are not implemented. The server does not acknowledge an inactive
schedule or report fictitious background snapshots. Other parameter names
return no GET match or an unknown-option SET error.

GET accepts multiple exact names and glob patterns. Names and glob matching
are case insensitive; an exact match preserves the supplied field spelling.
The first matching pattern chooses a result field's spelling, and overlapping
patterns do not duplicate fields. Empty/no-match queries return an empty map
(RESP3) or array (RESP2). Binary passwords retain their full bytes. Redis's
C-string handling of glob-pattern and boolean tails is retained where relevant;
exact parameter names still use their complete bytes.

Examples inside an authenticated administrative session:

```
CONFIG GET acl* requirepass databases
CONFIG SET acllog-max-len 256 requirepass replacement-password
CONFIG SET appendonly no save ""
```

Only users granted CONFIG GET can query these values, including the plaintext
requirepass compatibility value. As in Redis, granting CONFIG GET also grants
access to that sensitive value; the key-pattern ACL does not filter settings.
Neither ACL LOG nor CLIENT INFO/LIST records password/value arguments.

## Validation and atomic publication

SET validates all names, mutability and duplicate parameters before parsing
values. It then stages the complete change, including password storage. Errors
and allocation failures before publication leave all settings and ACL state
unchanged. Invalid integer syntax, negative limits, overflow, duplicate names
and immutable settings have reference-compatible errors. Unsupported RDB/AOF
activation has an explicit PKV error rather than a success response.

CONFIG commands use exclusive server execution admission. This serializes
configuration reads/writes with admitted KV and authentication commands, so
readers cannot observe a password from one SET and a limit from another.
The common preparation helper owns no live settings. Service publication uses
an already-staged default-user policy, a string swap and the audit limit update;
no allocation follows successful policy publication. Standalone callers must
hold the same admission or run before workers start.

The success reply is prepared before publication. Existing sessions remain
connected. CONFIG SET requirepass replaces only the default user's passwords;
its on/off state, command/key/channel permissions and selectors stay intact.
An unchanged requirepass configuration value does not overwrite a later
ACL SETUSER password edit. The remembered CONFIG value can therefore differ
from the default user's current ACL passwords. This also applies when startup
aclfile overrides frontend credentials.

The companion RPC transport retains its immutable startup requirepass/masterauth
policy. Frontend CONFIG SET does not rotate that transport secret. Runtime
changes are not automatically persisted to pkv.cf or aclfile. ACL SAVE stores
the current ACL user/password hashes, rather than CONFIG's remembered plaintext.

CONFIG SET inside EXEC takes effect in sequence and uses ordinary per-command
storage execution under exclusive EXEC admission. Such a transaction does not
use one native page-workspace commit. Read-only CONFIG GET does not disable
workspace eligibility. Queue-time syntax/arity handling, child authorization
and execution-time failures retain the normal transaction rules.

## Audit limit changes

A runtime reduction changes the configured limit immediately, but retains old
log entries until a later distinct event is inserted. Coalescing an existing
event does not trim the log. Setting zero similarly retains existing entries
until the next denied/authentication-failed request clears them, without
constructing a new event. ACL LOG RESET can clear them immediately. These
lazy cleanup rules match the local Redis reference. Startup capacity remains
configured through the existing integer configuration table; runtime limits
use the native-long range. Audit retention is per process, not per database.

## Verification and remaining scope

The native fixture tests rule validation and 32 preparation allocation budgets,
then 512 service-publication budgets. Failed attempts must preserve the old
configuration, default-user snapshot and audit limit. It also checks binary
passwords, unchanged-value behavior, retained permissions/OFF state and the
separate RPC startup policy. Audit tests cover deferred trimming and updates
racing reset/record operations; a standalone TSan fixture instruments that core.

Network tests compare stable observations with the local modified Redis 8.6.2
reference. They cover pattern/name case and deduplication, field spelling,
errors, multi-parameter rollback, password changes, ACL child permissions,
RESP2/RESP3 and EXEC. Four connected readers race 100 password/limit changes;
every result must be one complete published pair. Unsupported persistence
activation must fail without changing either configuration or credentials.

The final eight-configuration matrix passed **830/830 CTests**: memory and
C++11 memory 94 each, oneTBB and C++11 oneTBB 103 each, RocksDB Debug/Release
and WiredTiger Debug/C++11 109 each. Debug uses ASan/UBSan. WiredTiger runs
were isolated; other matrix groups used at most two configurations concurrently.
Existing timeouts, concurrency assertions and synchronous disk settings were
preserved. All 69 tracked source fingerprints stayed fixed during the matrix.
The current native audit TSan fixture passed; it does not establish whole-server
or fiber-library race freedom.

The final differential fixtures retain 31 CONFIG observations, 257 typed RESP3
comparisons, 72 ACL, 72 AUTH, 121 CLIENT INFO/LIST and 32 CLIENT KILL assertions,
plus the native OOM, log-capacity, transaction and concurrency checks above.
Metadata compares all advertised CONFIG GET/SET/HELP child rows exactly with
the corresponding reference entries.

Three rotating rounds checked 37,748,736 GET responses with identical C++11
Release core flags against the retained ACL LOG revision. Median rates were
about 1.477 million/s for old RESP2, 1.473 million/s for new RESP2 (-0.31%) and
1.453 million/s for new RESP3 (-1.62% relative to old RESP2). The workload uses
two server threads, four clients, 256-request batches, one 64-byte hot value
and disabled active expiry. This is not a significance or general throughput
claim, does not isolate a cause for the differences, and does not compare stock
Redis. See [raw samples and flags](benchmarks/config-get-2026-11-01.json).

Full matrix, sanitizer and source/binary evidence is recorded in
[the validation report](benchmarks/config-validation-2026-11-01.json).
The public subcommand metadata is taken from the reference and advertises only
GET/SET/HELP/RESETSTAT. No stored root/page/value encoding changes in the
configuration foundation.

Other Redis settings, live transport-secret rotation, Redis RDB/AOF support,
CONFIG REWRITE, complete replication and remaining command families
are still unfinished. Upstream interface references:
[CONFIG GET](https://redis.io/docs/latest/commands/config-get/) and
[CONFIG SET](https://redis.io/docs/latest/commands/config-set/).

Replica client-write policy is now available through CONFIG GET/SET under
`replica-read-only` and its `slave-read-only` alias. Both default to `yes`;
primary writes are unaffected. See [replica write admission](
REPLICA_READONLY.md) for startup and EXEC semantics.
