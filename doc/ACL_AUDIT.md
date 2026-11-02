# ACL security events

PKV implements `ACL LOG [count | RESET]` for public Redis authentication and
command/key/channel permission failures. It complements
[users and permissions](ACL_PERMISSIONS.md) and
[file persistence](ACL_PERSISTENCE.md).

The default query returns up to ten recent records. A signed integer limits
the result; nonpositive counts return an empty array. RESET clears records
without resetting entry IDs. Authentication syntax errors, NOAUTH errors,
successful requests and ACL DRYRUN do not create events. AUTH/HELLO failures
record the attempted username and command token, never the password.

Each entry has count, reason, context, object, username, age-seconds,
client-info, entry-id, timestamp-created and timestamp-last-updated fields.
Creation/update timestamps use Unix milliseconds.
RESP2 uses alternating field/value arrays; RESP3 uses maps and a double for
age-seconds. Objects and usernames are binary safe. Command denials use the
canonical command/subcommand name; key/channel denials preserve the offending
key or channel.
Contexts currently include toplevel and multi, including queue-time denial
and permissions changed before an EXEC child runs.

A matching event among the most recent ten records coalesces when its last
update is within 60 seconds of the event, and moves to the front. Matching considers reason, context, object and username.
The original ID/creation time stay fixed while count and client-info update.
IDs start at zero per process and are not reused by RESET, eviction or capacity
changes. A delayed worker cannot move an existing record's update time backward.
Wall-clock adjustments can still affect displayed age; this is not a monotonic
clock or a durable event journal.

The command schema and common behavior are checked against the local modified
Redis 8.6.2 reference. Upstream reference: [ACL LOG](
https://redis.io/docs/latest/commands/acl-log/).

## Capacity and concurrency

The startup setting is Redis's `acllog-max-len`, with default 128:

```
acllog-max-len = 128
```

Use it inside the existing `service pkv` block. Zero disables retention and
skips event construction. Negative values fail startup. The oldest records
are evicted after a new distinct entry exceeds the configured count. This
bounds entry count; individual binary object/username fields are retained
in full, rather than silently truncated. Payload bytes are not separately
capped. The log is process-local and shared by workers; it is not per database
and does not survive restart or participate in ACL SAVE/LOAD.

Successful requests take no audit mutex and do not construct audit entries.
Failed requests build a complete candidate before acquiring the log
mutex. Updating/promoting or inserting an entry then uses nonallocating list
operations. Snapshots, reset and capacity changes serialize on that mutex.
No network/storage operation runs under it. The client-info formatter is shared
with CLIENT INFO/LIST and reads their existing lifetime-safe client views.
Only metadata and resource sizes are included, not command arguments or values.

Allocation failure during event construction leaves retained entries intact
and increments an internal dropped-event counter. The request remains denied;
audit failure cannot grant access. The counter is available to native code,
but public INFO metrics for it are not implemented. This makes the log
best-effort under memory failure, not a lossless security audit facility.
Counts saturate at the signed response limit. Once entry IDs are exhausted,
new distinct entries are dropped rather than reusing IDs.

## Verification and remaining scope

Native tests cover the ten-entry scan boundary, grouping window, binary fields,
latest client metadata, promotion, count limits, eviction, reset IDs, disable/
reenable and delayed arrival. Thirty-two allocation budgets cover new and
matching events; four readers race eight writers producing 8000 events, then
recording races reset and capacity changes. A standalone TSan build instruments
the core audit class and fixture. It does not prove whole-server race freedom.
A retained variant without the timestamp guard fails the delayed-writer probe.

Network tests compare 18 stable observations with the reference and verify
RESP2/RESP3 types, AUTH/HELLO failures, binary keys, command/subcommand denial,
queue and EXEC contexts, DRYRUN exclusion and increasing IDs after reset.
They exercise default 128-entry eviction and ten-entry queries, custom capacity,
zero retention, negative startup rejection and NOAUTH/syntax exclusions.
Eight clients generate 800 denials that must form one exact-count entry.

The complete core candidate passed **814/814 CTests**: memory/C++11 memory
92 each, oneTBB/C++11 oneTBB 101 each, RocksDB Debug/Release and WiredTiger
Debug/C++11 107 each. The final revoked-identity diagnostic correction then
passed **56 focused CTests** across all eight configurations: native policy,
network ACL, ACL file, ACL LOG, RESP3, AUTH and CLIENT KILL. It adds the canonical
command/subcommand object without changing authorization verdicts. Native and
socket probes retain the failing pre-correction behavior; final self-removal
inside EXEC must log `set` and leave the denied write absent.

Full and focused phases have separate source/binary fingerprints and logs.
The final native test entry line was wrapped within 80 columns after focused
checks; Clang C++11 preprocessing produced identical token streams. New code
fits 80 columns and functions stay below 200 lines. Existing larger legacy
test functions were not rewritten. Two standalone TSan fixtures (audit and
final policy) passed. Debug matrix builds use ASan/UBSan. Matrix runners use at
most two configurations concurrently; WiredTiger runs are isolated. Existing
timeouts, concurrency assertions and disk synchronization remain unchanged.

Three rotating rounds checked 37,748,736 GET responses against the retained
ACL persistence revision with identical C++11 Release core flags. Median rates
were about 1.463 million/s for the baseline RESP2, 1.466 million/s for new RESP2
(+0.19%) and 1.430 million/s for new RESP3 (-2.24% relative to baseline RESP2).
This uses two server threads, four clients, 256-request batches, one 64-byte
hot value and disabled active expiry. It does not isolate a cause for the
RESP3 difference, establish statistical significance, measure mixed workloads
or compare with stock Redis. Raw samples and flags are in
[the measurement](benchmarks/acl-log-get-2026-11-01.json).

Final matrix, sanitizer and revision-performance evidence is recorded in
[the validation report](benchmarks/acl-log-validation-2026-11-01.json).
No root/page encoding or bounded value representation changes in this revision.

Local [Pub/Sub](PUBSUB.md) now records channel permission denials, including
subscription, publication and queued-command checks. Scripting and modules have
no public runtime yet, so lua/module events are not claimed. TLS certificate authentication and RPC
transport-failure events remain unfinished. [Runtime CONFIG SET/GET for
capacity](CONFIGURATION.md) is now available. Durable export and dropped-event
INFO metrics remain unfinished. Full Redis compatibility remains the active
goal.
