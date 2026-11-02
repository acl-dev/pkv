# ACL users and runtime permissions

This revision adds named users and command/key authorization to the existing
AUTH, HELLO and default requirepass support. Implemented ACL children are CAT,
DELUSER, DRYRUN, GENPASS, GETUSER, HELP, LIST, LOAD, LOG, SAVE, SETUSER, USERS
and WHOAMI. COMMAND metadata advertises these children only. Startup aclfile
and SAVE/LOAD are covered in [ACL persistence](ACL_PERSISTENCE.md). Security
events are described in [ACL audit](ACL_AUDIT.md). Full Redis compatibility
remains the active goal.

## Rules and identities

SETUSER stages the complete rule list before publishing an immutable policy.
An invalid modifier or allocation failure leaves the prior policy unchanged;
a failed new-user request publishes no user. Administrative ACL commands use
exclusive execution admission. Authentication and ordinary KV dispatch use
shared admission; CLIENT retains its existing connection-only admission.
No policy-table mutex spans command execution or network/storage IO.

Supported rules include on/off, nopass/resetpass, plaintext add/remove and
SHA-256 add/remove, command/category/subcommand rules, additive first-argument
rules for commands without subcommands, key read/write modes, channel patterns,
root/extra selectors, clearselectors and reset. Usernames and patterns reject
whitespace and NUL. Password bytes are binary safe. The parser accepts selectors
in one argument or spread over multiple arguments, including quoted tokens.

A selector must allow the command and all of its keys. Distinct selectors
cannot contribute different pieces of a multi-key authorization. For each key,
one matching pattern must provide all required access modes; identical patterns
merge their modes, while different overlapping R/W patterns cannot combine.
The access/update/insert/delete flags come from the existing command metadata.
WATCH requires a matching key pattern without reading or modifying key data.
SCAN/KEYS results are not post-filtered by ACL patterns: granting these commands
permits key-name discovery, as in Redis.

SET key discovery follows the reference's conservative scan of trailing tokens:
a literal GET in a condition value can still require read permission. This
matches COMMAND key flags and reference ACL behavior. It is intentionally not
replaced by a more permissive interpretation of the value as an operand.

Policies contain password hashes rather than frontend plaintext passwords.
The SHA-256 implementation has standard empty/abc/multi-block/million-a vectors
and binary-password checks against Python hashlib. Hash comparison scans all
bytes. GENPASS reads OS entropy from /dev/urandom, rounds bit counts up to
hexadecimal digits, and fails if the secure source is unavailable. It never
falls back to a general-purpose random generator. Non-POSIX entropy integration
remains unimplemented.

A retained identity has an immutable numeric generation. DELUSER revokes it
and closes its connections; creating the same username produces a new identity
and cannot revive old sessions. All user/connection snapshots and reply storage
are prepared before revocation/shutdown. Self deletion sends its reply before
closing; subsequent pipeline roots are suppressed. CLIENT KILL USER matches
identity generation, and CLIENT INFO/LIST report the actual username.

OFF blocks new logins while preserving existing authenticated sessions, as in
Redis. Password changes also preserve existing sessions. Failed AUTH/HELLO does
not replace the identity; HELLO stages protocol/name/user changes together.
RESET restores the current default user, DB zero and RESP2. New/reset sessions
are authenticated automatically only if default is both on and nopass.
Ordinary commands from an existing unauthenticated session also consult the
current default on/nopass state, within execution admission. This temporary
access does not promote the session's authenticated flag: requiring a password
again blocks it and aborts its queued EXEC. HELLO still requires actual
authentication or its AUTH option, matching the reference's separate check.

## Fast path and policy lookup

A default/unrestricted connection checks immutable identity ownership plus
atomic revoked/unrestricted flags. It performs no policy-table lock, shared
policy snapshot, key extraction or glob matching. Restricted sessions load a
coherent immutable snapshot and use an immutable command-name/parent index;
they do not scan the full command catalog on each request. The index is built
before workers start. Administrative user lookup/publication remains protected
by its own short mutex; the KV backend and oneTBB data path are unchanged.

## Transactions and storage

Authorization occurs before queueing and again for each EXEC child. A queue-time
failure marks EXECABORT. A later revocation produces a NOPERM child while other
allowed children retain their order. ACL changes inside EXEC affect subsequent
children. Authentication/reset/quit commands retain their NOAUTH exemption.

Workspace planning skips currently unauthorized commands, including their
SELECT effects and legacy-source inspection. Transactions containing ACL,
AUTH or HELLO use ordinary per-command storage execution because they can
change the policy/identity during execution; exclusive EXEC admission still
prevents other admitted KV commands from interleaving. They do not receive the
single-workspace native commit guarantee. The existing supported single-DB KV
transactions retain that workspace path. No stored data encoding, root/page
size or bounded List/Hash/Set/ZSet representation changes.

## Remaining scope

Users can now be saved to and loaded from aclfile. Runtime changes require
explicit SAVE to survive restart. ACL LOG now exposes recent events.
Scripting, modules and named-user replication authentication remain pending.
Local Pub/Sub now enforces channel rules in the same selector as command
permission. PSUBSCRIBE requires the exact authorized pattern; publishing checks
its channel rather than its payload. Policy updates revalidate existing
memberships and close affected connections, including after AUTH changes.
See [Pub/Sub ACL semantics and limits](PUBSUB.md).
CAT and command rules describe the implemented command catalog. Categories
with no implemented command are not advertised. Canonical LIST/GETUSER rules
are equivalent replayable permissions; compression/order can differ from Redis.

The companion RPC transport retains its separately configured startup
requirepass/masterauth policy. Changing frontend default-user passwords via
ACL does not reconfigure that transport. This is not Redis PSYNC or a complete
replication/security implementation. Existing authentication transport limits
are documented in [AUTH](AUTH.md).


## Runtime ACL validation (2026-10-31)

The following phases and measurements describe the earlier runtime ACL
revision. Current file persistence validation is recorded separately in
[ACL persistence](ACL_PERSISTENCE.md).

The complete eight-configuration matrix validates the retained core candidate.
Focused follow-up checks validate the final denied-key diagnostic selection,
binary-safe DRYRUN bulk text, and dynamic default-user authentication gate.
The old implementations fail dedicated regression probes; the final checks
run native ACL, socket ACL, RESP3 and AUTH in every configuration. Source and
binary fingerprints distinguish the phases in the validation report. Passing
these checks does not establish complete Redis compatibility or whole-server
race freedom.


The retained candidate passed 782/782 full-matrix CTests across eight memory,
oneTBB, RocksDB and WiredTiger configurations. Final corrections passed 40/40
focused CTests across those same configurations. The final socket fixture
compares 72 common assertions with the local modified Redis 8.6.2 reference;
RESP3 retains 251 typed comparisons, AUTH 72, CLIENT INFO 121 and KILL 32.
Four standalone TSan fixtures passed. These are separate phases with separate
source/binary fingerprints; the complete matrix predates the focused fixes.
See [validation and retained negative probes](
benchmarks/acl-validation-2026-10-31.json).


## Checked GET revision comparison

Three rotating rounds checked 37,748,736 GET replies across the retained
CLIENT INFO revision (RESP2) and this final ACL revision (RESP2/RESP3). Both
use identical C++11 Release core flags, the memory backend, two server threads,
four clients, batches of 256, a 64-byte hot value and disabled active expiry.
Median rates were about 1.484 million requests/s for the baseline, 1.465 million
for new RESP2 (about -1.26%) and 1.463 million for new RESP3 (about -1.36%).
The observed cost is limited to this default-user hot-key GET workload; it does
not measure restricted-user contention, mixed workloads or other backends,
and has no statistical-significance or stock Redis speed claim. Raw samples,
binary hashes and matching flags are in [the measurement](
benchmarks/acl-get-2026-10-31.json).

An exploratory native check comparison motivated the immutable name/parent
index: five samples of 200,000 restricted GET permission checks observed a
median near 489 ns before and 87 ns after indexing. Those separately measured
samples were not interleaved and predate the final diagnostic correction.
Retained binaries/logs are fingerprinted in the validation report; this is not
an end-to-end server throughput measurement or a general scaling claim.
