# String digests and conditional deletion

PKV now implements DIGEST, SET IFDEQ/IFDNE and DELEX with all four value/digest
conditions, using the existing typed String/page transaction paths. The command
behavior is compared with the local modified Redis 8.6.2 binary and the official
[DIGEST](https://redis.io/docs/latest/commands/digest/),
[SET](https://redis.io/docs/latest/commands/set/) and
[DELEX](https://redis.io/docs/latest/commands/delex/) references.

DIGEST returns a lowercase, zero-padded, 16-character XXH3-64 digest of the
String bytes. A missing or expired key returns null; a live non-String returns
WRONGTYPE. TTL is not part of the digest. Decimal Strings produced by integer
operations hash their canonical decimal bytes, matching Redis integer objects.

SET IFDEQ updates an existing String with a matching digest. IFDNE updates a
missing key or a String with a different digest. GET and expiry options compose
with these conditions as with IFEQ/IFNE. Matching ignores hex letter case.
Digests are comparison tokens, not cryptographic authentication values.

DELEX without a condition deletes one live key of any supported type. With a
condition it requires a live String: IFEQ/IFNE compare bytes and IFDEQ/IFDNE
compare digests. It returns 1 only for a committed live-key deletion, otherwise
0. A skipped condition preserves the live value, TTL and WATCH state. A
successful deletion clears owned pages, updates DBSIZE/WATCH and publishes the
normal deletion through the existing replication stream.

## Validation order and compatibility

Digest length is checked only when a live String must be compared, after the
GET/type checks. Redis checks exactly 16 bytes but does not require all bytes
to be hexadecimal: a 16-byte nonhex token simply cannot match. These details
are reproduced, including absent-key SET IFDEQ returning null and absent-key
IFDNE creating a value even with a short token. GET still returns the old value
when a valid comparison fails.

DELEX checks arity first. Missing/expired keys return 0 before validating the
condition; a live non-String with any condition returns Redis's dedicated
String-condition error before checking the condition name or digest length.
Unknown conditions on a live String return the condition error. This ordering
is covered independently from successful operations.

Option tokens remain binary-length checked. PKV rejects NUL-suffixed condition
names. SET also retains the documented mutual exclusion of NX/XX and all four
comparison conditions in either order; the local reference accepts some
comparison-then-NX/XX combinations. These strictness differences remain explicit
and are not included in claims of identical differential replies.

The reference also sends two RESP replies (old value, then error) when SET GET
compares a live String against a digest of invalid length. PKV sends one error
and preserves the value. Tests require the following PING to receive PONG,
without a leftover response. This protocol integrity difference is tested
separately from common differential cases.

COMMAND INFO/LIST/DOCS and key discovery include the two new commands. DELEX's
GETKEYSANDFLAGS result uses dynamic removal/read-write flags, matching the
reference even for malformed argument lists. These argument-only metadata
queries never inspect keys or execute deletion. Cluster routing considers only
the actual key; comparison operands are values even if they contain hash tags.

## Memory, atomicity and failure handling

Native digests stream page-sized chunks through one XXH3 state. They do not
assemble a second full String. SET with GET hashes the already returned old
value, avoiding another page pass. The GET reply itself still needs memory
proportional to the old value. Legacy JSON roots retain their decoding cost;
live DIGEST and skipped conditions leave their original stored bytes untouched.
Custom codecs inheriting materializing range accessors retain that fallback.

Lookup, type checks, digest/value comparison and any mutation share the normal
logical-key transaction. Different keys retain the existing concurrency model;
there is no new global digest mutex or counter. A visited corrupt page, failed
allocation or rejected engine commit cannot publish a partial value/deletion.
Existing lazy expiry may still physically remove an already expired root.

Source EXEC sees staged values, can hash a preceding staged SET, and commits
eligible changes through one native source batch. Comparison operands and hash
states are transient; they are not persisted or sent to receivers. DIGEST and
failed live conditions do not cause value migration or WATCH invalidation.
No stored-format, wire-version or active-expiry configuration change is needed.

## xxHash dependency

The implementation uses the upstream xxHash 0.8.0+ public one-shot/streaming
APIs rather than reimplementing XXH3. Tested builds use 0.8.3. Configure an
installed development library with CMAKE_PREFIX_PATH or XXHASH_INCLUDE_DIR /
XXHASH_LIBRARY, or provide PKV_XXHASH_SOURCE_DIR containing xxhash.c and xxhash.h.
Source mode compiles a private PKV-namespaced library in the build directory,
avoiding symbol collisions with other dependencies. No external source tree is
modified. C++11 uses xxHash's own aligned-state allocator; PKV does not rely on
over-aligned C++11 new. The wrapper rejects headers older than 0.8.0.

The upstream source/installed-library choices produce the same known digest
vectors. The source-mode library is also ASan/UBSan instrumented in sanitizer
builds. An installed external binary library retains its own instrumentation.
See the [xxHash license](licenses/xxhash.txt) for binary distribution notices.
No third-party algorithm body is copied or reformatted into PKV source files;
new PKV code follows the 80-column and 200-line function limits.

## Verification and remaining scope

The C++ fixture checks known XXH3 vectors, 21 length boundaries and five stream
chunkings, including empty, short, medium, long and multi-page input. A 4-MiB
native value is digested, compared and deleted with a 64-KiB per-allocation
ceiling; GET hashes reuse one page read pass. It checks corrupt visited pages,
commit rejection, allocation rollback, legacy no-op bytes, the legacy JSON DAO,
owned-page removal and eight-thread single-winner deletion. oneTBB builds repeat
applicable cases on both memory engines.

The TCP fixture compares digest values, replies and subsequent data/TTLs with
local Redis, including integer encodings, malformed tokens and error priority.
It also checks WATCH, source EXEC, logical DBs, single-winner deletion, COMMAND
metadata, cluster routing and a real source-to-receiver conditional update and
physical deletion. A readiness write establishes the RPC stream before these
live-delivery assertions; it does not test initial synchronization.

These checks do not establish complete Redis parity, initial synchronization,
durable replay, crash safety, or general throughput superiority. Digest
comparisons can collide by design and are not cryptographic security checks.


The final eight-configuration matrix passed 610/610 CTests, with 1524 common
TCP differential assertions per build plus independent framing/strictness,
WATCH, race, cluster and live replication checks. All final sessions reached
terminal success. See the
[test matrix](../tests/README.md#string-digests-and-conditional-deletion) and
[source/reference identities](benchmarks/string-digest-validation-2026-10-31.json).
