# ACL file persistence

PKV supports startup `aclfile`, `ACL SAVE` and `ACL LOAD`. The file contains
named frontend users and their permissions; it is separate from the existing
RPC startup requirepass/masterauth policy. This does not complete Redis ACL,
replication or server compatibility. Runtime rules are described in
[ACL permissions](ACL_PERMISSIONS.md).

## Configuration and lifecycle

Set `aclfile` in the existing `service pkv` configuration block:

```
aclfile = /absolute/path/users.acl
```

Create the file before starting PKV. For example, a file that disables default
access and grants a password-protected application read access can contain:

```
user default off resetpass -@all
user operator on >choose-a-private-secret ~* &* +@all
user application on >another-private-secret ~app:* +get +mget +exists
```

Prefer password hashes for managed files. `ACL SAVE` writes hashes, never
plaintext passwords. The example's secrets must be replaced before use.
The configured file must load successfully before data databases open or
workers serve requests. Missing files, invalid rules and duplicate usernames
fail startup. An empty/unset path retains startup requirepass behavior, but
SAVE/LOAD return the reference's unconfigured-file error.

When aclfile is configured, it defines frontend credentials in place of
requirepass. In particular, an empty file or a file omitting `default` creates
the Redis-compatible built-in default user: on, nopass and unrestricted.
To prevent that fallback, explicitly include a restricted or disabled default
user. Frontend file changes do not alter RPC startup credentials.

SETUSER/DELUSER modify memory only. SAVE explicitly persists the current user
table; LOAD explicitly replaces it from the configured file. There is no
CONFIG SET aclfile, CONFIG REWRITE, automatic save or file watcher.

## Replacement and active connections

LOAD parses and validates the complete file, then prepares the new user table,
identity updates, removed-user connection targets and success reply before
publishing. Invalid input or allocation failure during preparation leaves the
old table and identities unchanged. A revision check rejects a stale plan.
The final policy publication allocates no memory. Server execution admission
excludes other admitted key commands during administrative ACL operations.
The standalone policy API publishes each retained identity atomically; it does
not promise a simultaneous multi-user snapshot without server admission.

Existing usernames keep their immutable identity generation. Existing
sessions immediately use the new permissions, while OFF and password changes
only affect new authentication. Removed users are revoked and their existing
connections close; recreating the name cannot revive those sessions. When
LOAD removes its own caller, the reply (or complete EXEC response) is sent
before closing, and following pipeline roots are suppressed.

An EXEC containing ACL operations retains exclusive execution admission but
uses ordinary per-command storage execution. LOAD affects subsequent children,
including their permission checks. Such an EXEC does not have the single
native workspace commit guarantee of supported single-DB KV transactions.

## File representation and IO failures

Each nonblank line is a `user` declaration. CRLF and a final line without LF
are accepted. Usernames and patterns retain the existing whitespace/NUL
restrictions; quoting preserves otherwise supported quotes and backslashes.
PKV round-trips its own quoted output. Not every quoted edge case is portable
to the local Redis 8.6.2 loader, which splits file lines differently.
Comments are rejected for the 8.6.2 compatibility baseline. Redis documentation
introduces comment support in 8.8; this implementation does not claim it.

Command rules preserve ordered category intent, such as `+@all -@admin`,
rather than expanding exclusions into today's command names. Reloading after
a command-catalog extension therefore reapplies the intended categories.
Key/channel patterns and additional selectors also survive SAVE/LOAD.

SAVE creates a mode-0600 sibling temporary file, handles short/interrupted
writes in bounded chunks, fsyncs the file, closes it, renames it over the target,
and fsyncs the parent directory. POSIX file/directory descriptors are marked
close-on-exec. Before rename, failure leaves the original target intact and
cleans up the owned temporary file. A failed temporary creation never deletes
an unrelated file matching the template name.

A parent-directory fsync failure occurs after replacement: SAVE reports an
error and logs `replacement visible=1`, although the new target is already
visible. Its crash durability is then uncertain. Operators must inspect the
file and storage error before retrying. Successful saves replace inode metadata
with the new private file; existing mode/owner/ACL metadata is not copied.
POSIX IO is implemented; a native Windows save path remains unfinished.

## Validation and reference limits

The native fixture injects allocation failure at 512 preparation budgets and
commits a staged replacement with an allocation budget of zero. It checks
stale/foreign plans, stable/revoked identities, category/quoted round trips,
and four readers during 2000 reloads. File fault injection covers directory
and temporary opens, ENOSPC, short/zero/overreported writes, EINTR, file fsync,
close, rename and directory fsync, including original-file preservation and
cleanup. It also verifies descriptor flags, 0600 mode and bounded writes.

The network fixture exercises configured/unconfigured commands, startup
failures, RESP2/RESP3, password hashes, restarts, invalid-file atomicity,
retained OFF sessions, removed-user closure, default fallback and self-removal
inside EXEC. Common cases also run on the local modified Redis 8.6.2 reference.
A standalone malformed-selector probe makes that reference exit with SIGSEGV;
PKV must reject the same input without changing live permissions. The crash is
retained as reference evidence, not reproduced in every differential run.

The final eight-configuration matrix passed **798/798 CTests**: memory and
C++11 memory 90 each, oneTBB and C++11 oneTBB 99 each, RocksDB Debug/Release
and WiredTiger Debug/C++11 105 each. Debug builds use ASan/UBSan. WiredTiger
runs were isolated; other groups used at most two configurations concurrently.
Existing timeouts, concurrency assertions and disk synchronization remained
unchanged. Two current standalone TSan fixtures (policy and reload publication)
passed. They do not establish whole-server or fiber-library race freedom.

The final network fixtures retain 72 ACL, 253 typed RESP3, 72 AUTH, 121 CLIENT
INFO/LIST and 32 CLIENT KILL common differential assertions, plus the file
lifecycle checks above. One ACL child-list line was wrapped within 80 columns
after the first six configurations; Python AST equality proves no semantic
fixture change. No compiled source changed during the matrix.

Upstream semantics: [ACL LOAD](https://redis.io/docs/latest/commands/acl-load/),
[ACL SAVE](https://redis.io/docs/latest/commands/acl-save/) and
[ACL management](https://redis.io/docs/latest/management/security/acl/).
Final build, sanitizer and differential evidence is recorded in
[the validation report](benchmarks/acl-file-validation-2026-11-01.json).

No new revision throughput comparison is claimed. CMake reconfiguration
added explicit architecture/include flags relative to the retained baseline,
so the benchmark's identical-flags precondition is not met. Earlier scoped GET
measurements remain historical evidence in [ACL permissions](ACL_PERMISSIONS.md).

[ACL LOG](ACL_AUDIT.md) now exposes recent public authentication/permission
failures. Local [Pub/Sub execution](PUBSUB.md) now checks channel permissions
and ACL LOAD revalidates existing subscriptions. Named-user RPC replication
authentication remains unfinished. Bounded string/list/hash/set/zset storage
formats are unchanged.
