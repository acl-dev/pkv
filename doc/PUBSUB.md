# Pub/Sub commands and transport

PKV implements SUBSCRIBE, UNSUBSCRIBE, PSUBSCRIBE, PUNSUBSCRIBE, SSUBSCRIBE,
SUNSUBSCRIBE, PUBLISH, SPUBLISH and PUBSUB. The PUBSUB subcommands are HELP,
CHANNELS, NUMSUB, NUMPAT, SHARDCHANNELS and SHARDNUMSUB. Delivery currently
covers connections attached to this PKV process; cross-node propagation and
Redis protocol replication remain separate unfinished work.

## Membership and frames

A service-owned broker shares subscription state across worker threads and
logical databases. Channels are binary-safe names, independent of KV keys
and SELECT. Ordinary and pattern subscriptions share acknowledgement counts;
shard subscriptions have their own count. All three families determine
whether the connection is subscribed. Duplicate subscriptions still receive
an acknowledgement without adding membership. Unsubscribing an empty family
returns one acknowledgement with a null channel name.

PUBLISH delivers to direct subscribers and every matching pattern. Its count
includes each delivery, so a connection subscribed directly and through a
pattern contributes more than one. NUMPAT counts distinct registered
patterns, rather than client-pattern pairs. SPUBLISH and shard membership
are isolated from ordinary channels and patterns. Enumeration order is not
specified; channel glob matching preserves binary lengths and uses the
existing nonrecursive Redis-style matcher.

RESP2 notifications and acknowledgements are arrays. RESP3 uses push frames.
A RESP2 subscribed connection accepts the six subscription commands, PING,
QUIT and RESET. Its PING response is the two-element `pong` array. RESP3
permits ordinary commands while subscribed and retains the normal PING
response. HELLO can switch a subscribed RESP3 connection to RESP2; subsequent
commands then follow RESP2 restrictions.

## Ordering and execution

Membership mutations and message enqueue operations share the broker lock.
The broker never writes sockets or waits for network progress. Each promoted
connection has one writer fiber on its owning scheduler. Other threads only
enqueue immutable frames. A message is encoded once per needed protocol for
a direct channel or matching pattern and shared by its recipients.

Reply placeholders and captured prefixes preserve mutation boundaries:
notifications published before an unsubscribe takes effect precede its
acknowledgement, and later publications cannot reach the removed member.
Self-publications follow the PUBLISH integer reply. Within EXEC they follow
the entire outer reply, including when a later queued command unsubscribes.
Multiple subscription acknowledgements inside EXEC retain the local reference
server's wire shape through an internal raw-response annotation; incoming
requests cannot set that annotation.

HELLO captures previously queued external notifications before its reply and
changes the broker's protocol under the same membership lock. Deferred self
messages retain the encoding used when they were published. RESET clears all
subscriptions without generating unsubscribe acknowledgements, restores DB 0
and RESP2, and resets authentication/transaction state. QUIT sends its normal
reply before shutdown.

A real blocking command on a RESP3 subscriber does not reserve an unfilled
reply placeholder across its wait. Notifications therefore remain writable
while BLPOP or BZPOPMIN is blocked. Push completion also preserves the blocked
command's argument-memory observation. EXEC retains exclusive admission and
its queued blocking commands keep their existing nonblocking behavior.
Pub/Sub commands disable storage workspaces, using the existing sequential
EXEC path under exclusive admission instead of publishing side effects ahead
of a future workspace commit.

## Permissions and routing

One ACL selector must authorize the command and all channel arguments.
Ordinary and shard names match channel patterns; PSUBSCRIBE requires an exact
allowed pattern string. PUBLISH/SPUBLISH check the channel argument, not the
message payload. Unsubscribe commands require command permission but do not
require permission for the channels being removed. Channel failures produce
NOPERM replies, channel audit records and the channel-denial INFO counter.
ACL DRYRUN includes channel diagnostics.

Existing memberships are validated using the union of channel permissions
across selectors, independently of command permissions. Successful SETUSER
updates revalidate clients of the affected existing identity, including when
channel-rule text is unchanged. AUTH can retain subscriptions established
under the old identity; a subsequent policy update can therefore disconnect
the connection. Updating another identity does not close it. ACL LOAD also
revalidates retained identities. The executing client receives its reply
before a required self-close; other affected connections are interrupted.

Channels are not KV arguments. COMMAND GETKEYS/GETKEYSANDFLAGS report no key
arguments for the subscription/publication commands, including the shard
commands whose metadata contains NOT_KEY specifications. Key ACL rules,
WATCH, keyspace counts and persisted values do not use these channel names.

Ordinary Pub/Sub operates without assigned cluster slots. SSUBSCRIBE checks
same-slot channels and their owner; SPUBLISH routes by its channel. Their
queued transaction admission also includes channel routing. SUNSUBSCRIBE
remains local and accepts different slots or unserved slots. Full cluster
fanout, topology transitions and publication through the private replication
stream are not implemented. Local publications are allowed on read-only
replicas; KV writes remain rejected, with READONLY taking precedence over
RESP2 subscription-mode errors.

## Resources and diagnostics

The writer is created lazily by a subscription-family command. Never-promoted
clients and publication-only clients retain their direct output path.
Connections that have used subscriptions retain the writer until close.
Payload capacity is reserved before encoding, avoiding trailer-driven buffer
growth. Native fanout tests verify that 128 recipients share exactly two
protocol frames for a 1 MiB publication.

While subscribed, the hard quota is 32 MiB for both retained payload bytes
and estimated C++ queue memory, including empty nodes, string capacity and
the active frame. Removing the last subscription or RESET switches the
transport to the default unlimited normal-client policy; re-subscribing
restores the Pub/Sub bound. The mode callback runs before membership becomes
visible to another publisher, after releasing the inspector's count lock.
A 33 MiB binary ECHO regression checks large normal replies after both exits.
Shared payloads are conservatively charged to each recipient. Overflow or
queue allocation failure interrupts the slow connection without waiting for
its socket. Redis's configurable hard/soft output-buffer-limit policy is not
yet implemented. Pub/Sub has no delivery persistence, replay or acknowledgement
from receiving applications. An allocation failure during a publication does
not roll back frames already enqueued to earlier recipients.

CLIENT INFO/LIST report P, sub/psub/ssub and queued output observations.
TYPE normal/pubsub filters apply to LIST and KILL. INFO exposes pubsub_clients,
pubsub_channels, pubsub_patterns and pubsubshard_channels. Memory observations
are estimates of local ownership, not copies of Redis allocator statistics;
ACL allocations, allocator rounding and fiber stacks are outside the queue
quota. Transport ownership and natural writer shutdown are described in
[the output transport notes](PUBSUB_OUTPUT.md).

## Verification and scope

`pubsub` tests frames, counters, protocol boundaries, selector unions,
foreign endpoints, quotas, concurrent publishing, shared fanout and 1,152
allocation budgets with rollback of both membership indexes. `pubsub_commands`
checks typed socket parity, pipelines/EXEC, logical databases, CLIENT/INFO,
blocking subscribers, live ACL updates, slow readers, concurrent membership
changes, cluster admission and read-only replicas.

The local Redis 8.6.2 reference is marked modified and is identified by hash
in the validation artifacts. It is not an unmodified official performance
baseline. Repeated reference probes found inconsistent handling of
`PUBSUB numsub\0bad`: 52 of 64 fresh processes rejected it and 12 treated it
as NUMSUB. Its command dictionary hashes the full SDS length but compares
names using C-string case comparison. PKV consistently rejects NUL-suffixed
command/subcommand names. Fourteen checks protect that behavior separately
from the differential count; binary channel names and message payloads remain
supported.

`pubsub_handler` also injects 64 allocation failures into the first SUBSCRIBE
handler and checks that recovered ordinary connections can return 33 MiB
replies. Output starts in normal mode and adopts the subscribed quota only
when a subscription exists. A response object is reserved before membership
changes; completed acknowledgement frames move into it without a second copy.

Darwin arm64 sanitizer builds use a private ACL archive with sanitizer fiber
stack registration. This resolves alternate-stack exception-unwind diagnostics
without disabling checks; a deliberate overflow remains detectable. The
[testing notes](../tests/README.md) document the reproducible builder and scope.
Release builds continue to use the original ACL archive.

The Pub/Sub integration revision passed 1,056 CTests across eight MDB, oneTBB, RocksDB and
WiredTiger configurations, including C++11 and ASan/UBSan builds. Four focused
fixtures each passed five additional repetitions. Each full configuration
checks 285 typed Pub/Sub differential observations, 14 independent binary-name
safety cases, 1,152 membership allocation budgets and 64 first-subscription
handler budgets. Existing RESP3 discovery checks now cover 325 typed
observations. That revision's 552-source freeze, binaries, runtime archives and logs
are identified in [the validation report](
benchmarks/pubsub-validation-2026-11-01.json).

Three rotating rounds per backend checked 100,663,296 GET replies in total.
Baseline and candidate use identical C++11 Release flags and the original ACL
archive, two server threads, four clients, 256-request batches, one 64-byte hot
value and disabled active expiration. Each protocol is compared with itself.
Median MDB changes were +1.24% for RESP2 and -2.06% for RESP3; oneTBB changes
were +0.65% and +0.55%. These three rounds do not establish statistical
significance, explain the differences or compare against official Redis.
See raw [MDB](benchmarks/pubsub-get-mdb-2026-11-01.json) and
[oneTBB](benchmarks/pubsub-get-tbb-2026-11-01.json) samples and fingerprints.

The isolated command inventory found 228 advertised PKV names and 412 reference
names: 185 missing reference names and one extra PKV alias, cluster|addslave.
Names include subcommands; presence alone does not prove complete semantics.
The [inventory](benchmarks/pubsub-command-inventory-2026-11-01.json) records all
names and binary hashes. Streams, scripting, standard Redis replication,
cross-node Pub/Sub propagation and broader administration remain unfinished.
