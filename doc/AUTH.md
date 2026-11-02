Current [CONFIG SET requirepass](CONFIGURATION.md) changes frontend default
credentials at runtime while retaining the separate startup RPC policy.
The earlier startup-only statements below describe the original transport
revision and are superseded for frontend configuration.

Current named-user and runtime permission support is described in
[ACL permissions](ACL_PERMISSIONS.md). This document retains the earlier
startup requirepass/masterauth transport design and its validation history.
Frontend ACL passwords can now change at runtime; the separate RPC policy
still uses startup configuration. [ACL file persistence](ACL_PERSISTENCE.md)
now supports startup loading and explicit SAVE/LOAD. [ACL LOG](ACL_AUDIT.md)
reports recent public authentication/permission failures; TLS remains
unfinished.

# Default-user authentication and protected RPC streams

PKV supports AUTH with either a password or `default password`, together with
requirepass/masterauth startup configuration. The reference is local modified
Redis 8.6.2. This adds default-user authentication, not user/ACL administration,
per-command/key permissions, TLS or dynamic credential configuration.

## Configuration and connection state

Add requirepass inside the existing service pkv block to protect Redis commands
and the companion RPC publisher. An empty or absent requirepass keeps the
existing unrestricted default/no-password behavior. masterauth supplies the
password for outgoing live replication and cluster synchronization connections;
it is independent of the receiver's own requirepass. For example:

```
requirepass = choose-a-private-value
masterauth = upstream-private-value
replica_rpc_source = 127.0.0.1:29001
```

These settings are loaded before workers start. There is no CONFIG SET password
rotation, ACL user database or masteruser support. Existing CONFIG GET coverage
is unchanged. Configuration and process memory contain the password in plain
text, and these connections do not add encryption or server certificates.
The credential handshake is not a substitute for TLS.

New protected connections start unauthenticated. Valid commands receive NOAUTH
before database access, cluster routing or execution admission, apart from
AUTH, HELLO, RESET and QUIT. Unknown commands, root arities and CLIENT child
arity checks retain their existing preflight precedence. Rejected KEYS/DBSIZE
requests do not take the global execution gate. Authentication is local to the
connection, applies across logical databases and is never replicated as a key.

AUTH validates the complete username/password byte lengths. Only the default
username exists; unknown names are rejected. A wrong attempt does not revoke a
previous successful login or clear the selected DB, name, WATCH or transaction
state. AUTH queues normally in MULTI; an over-arity request produces its body
syntax error at EXEC. Unauthenticated EXEC uses Redis's EXECABORT-wrapped NOAUTH
reply and clears transaction state.

Without a configured password, AUTH with only a password returns the reference
configuration error, while AUTH default password accepts the no-password user.
HELLO AUTH retains that default/no-password behavior. On protected connections,
HELLO without credentials requires a previous successful login. HELLO AUTH
checks credentials before committing protocol/name changes. RESET clears login
state on protected services and restores RESP2/DB 0/name/WATCH/queue state;
ID and library attributes remain intact.

Credential matching first checks the public lengths and then accumulates every
same-length byte difference without returning at the first mismatch. NULs are
not treated as string terminators. It does not allocate or copy the secret for
each comparison. No password hashing database or formal side-channel guarantee
is claimed. Ordinary authorized commands only check their connection flag;
there is no shared password lock or password scan on each key operation.

## RPC authentication and connection lifetime

The RPC listener must not leak data around the Redis-port guard. A protected
publisher reads `PKV-AUTH <byte-length>\r\n`, the exact password bytes and CRLF
before constructing/registering a subscriber. It acknowledges +OK only after a
match. Header parsing is capped at 64 bytes. Password bytes are read/compared
in 1-KiB fragments against the configured length, so an attacker cannot request
an arbitrary allocation or make the server materialize another password blob.
All incoming authentication reads share a monotonic five-second deadline;
second-granularity socket timeouts can affect the exact disconnect instant.

Only then can PKV-REPL 1/2/3 negotiate the existing page/EXEC wire formats, or an
auth-aware legacy subscriber consume legacy events. Old subscribers that do not
supply credentials cannot read protected data. Wrong/absent credentials and
malformed lengths are closed before subscriber registration, including an idle
or trickled connection. Outgoing masterauth is checked before protocol offers;
failed authentication never retries as an unauthenticated legacy stream.
Open services retain their existing unprotected/legacy behavior.

Testing exposed a real pre-existing lifetime defect: start-up code called the
receiver loop even after open failed and closed its ACL stream. The next raw
read entered the closed stream and crashed. The receiver now reopens unopened
streams, and the RPC reader rejects null/closed streams before calling ACL.
Zero-byte payload reads remain valid without a socket. Wrong credentials and
an initially unavailable upstream now leave the receiver serving Redis clients
while it retries. No database format, page version or durability setting changes.

## Validation and reference framing difference

The C++ policy test covers all 256 byte values, mismatch positions, username/
length/NUL boundaries, fragment bounds and a one-MiB comparison with allocation
disabled. Client-state tests check protected RESET and strengthen fault injection
by requiring each failed allocation to actually enter the replacement-new hook.
Failures now print statuses and hook counts without relaxing prior preservation
assertions. The old RocksDB allocation anomaly remains undiagnosed; 200 unchanged
fixture processes and a minimal three-hook probe passed before this change.

TCP checks compare 72 ordinary AUTH replies with the reference, plus separate
HELLO/RESET, WATCH, DB/name/ID retention, CLI RESP3 authentication and framing
assertions. Raw RPC cases require no unauthenticated data, authenticated version
1/2/3 readiness, a total deadline under trickled bytes and complete long-password
matching. A 4097-byte password exercises multiple fragments and rejects an
incorrect final byte. Live source/receiver tests cover wrong masterauth, large
Hash pages, source EXEC and successful long-password replication. An unavailable
upstream must not crash the receiver. These are live-update tests, not initial
sync, replay, failover or disk crash-recovery tests.

The local reference has a reply-framing defect: wrong AUTH inside EXEC omits
its error when another reply is pending. The retained raw EXEC declares five
children but emits only four, so a later PING can be consumed as the missing
child. PKV emits one error for each failed AUTH/HELLO AUTH, preserving complete
arrays and subsequent replies. Those cases are independent strict assertions,
not claimed as identical reference replies. Evidence is retained at
`/tmp/pkv-auth-reference-framing.log`; the initial receiver crash is retained at
`/tmp/pkv-auth-memory-third-target.log`.

Per-user ACLs, ACL LOG, TLS, rate/resource policies and runtime password changes
remain unfinished. The feature does not establish full Redis parity, overall
race freedom, replication recovery or throughput superiority over stock Redis.


Final full-matrix results passed 694/694 CTests: memory/C++11 memory 77 each,
oneTBB/C++11 oneTBB 86 each, RocksDB Debug/Release 92 each and WiredTiger
Debug/C++11 Release 92 each. Each configuration passed 72 ordinary AUTH
comparisons, 237 typed-protocol comparisons and authenticated redis-cli checks.
The final one-expression formatting fix was rebuilt in all eight executables
and followed by an additional authentication check per configuration. See
[validation metadata](benchmarks/auth-validation-2026-10-31.json) and the
[scoped GET comparison](AUTH_PERFORMANCE.md). These passing results do not
establish a root cause for the historical metadata fault-injection anomaly.
