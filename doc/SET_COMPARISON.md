# Conditional String updates

SET now accepts `IFEQ expected` and `IFNE expected` alongside its existing
NX/XX, GET and expiry options. These value conditions were added to Redis in
8.4; the reference is the [Redis SET documentation](
https://redis.io/docs/latest/commands/set/) and the local Redis 8.6.2 source.
The local reference binary reports a modified build, so differential tests
are evidence against that binary, not a certification against every release.

IFEQ updates only an existing String equal to the expected bytes. IFNE updates
an absent key or a String different from those bytes. Empty strings differ
from absent keys. Expired keys count as absent. A live non-String produces
WRONGTYPE for either comparison condition, including when GET is requested.
Failed conditions return null, or the old String when GET was specified.

NX, XX, IFEQ and IFNE are mutually exclusive. Repeating the same condition is
accepted, and the last comparison operand wins. Each comparison consumes one
binary-safe argument. Comparison operands may themselves spell option names.
Option names are case insensitive; PKV requires their complete byte length
and rejects NUL-suffixed tokens rather than accepting a C-string prefix.
Malformed options or expiry arguments fail before touching storage.

The local reference accepts a comparison followed by NX/XX in some cases,
while the reverse order fails. PKV follows the documented mutual exclusion
in both orders. This reference discrepancy and NUL tokens are separate
strictness tests, excluded from claims of identical differential replies.

## Atomicity and stored representation

Lookup, live-type validation, comparison and replacement share the existing
logical-key transaction. Failed conditions on live keys preserve their value,
TTL and WATCH state, and publish no replacement to replication. Existing lazy
expiry cleanup may still remove an already expired physical root. GET returns the old value even when the condition
fails. Successful updates preserve TTL only with KEEPTTL, otherwise applying
the requested expiry or clearing the old TTL as ordinary SET does.

Native Strings compare their root length first. Equal-length values are read
in at most 16-KiB chunks, stopping at the first mismatch. A length mismatch
reads no payload pages. The helper owns only a chunk, not another complete old
String. If GET already materialized the old value for the reply, comparison
reuses those bytes without a second page pass. GET's reply allocation is still
proportional to the old String length.

Legacy JSON roots use the existing decoded view's length/range accessors.
They retain their full-root JSON decoding cost. Custom codecs inheriting the
materializing range fallback may also retain full-value allocation costs.
A skipped comparison of a live key leaves legacy root bytes untouched.
Successful native SET replaces the result using bounded String roots and pages;
no new stored format or large serialized replacement is introduced.

The internal `set_options::comparison` pointer borrows a synchronous caller
argument. It must remain live until SET returns. It is never retained in a
workspace, persisted record or replication message. EXEC compares the staged
value visible at that command, and the final native source batch follows the
existing replication path. Corrupt visited pages and rejected engine commits
fail without publishing a partial replacement.

## Validation and remaining work

`set_compare` covers 4-MiB native comparisons with a 64-KiB per-allocation
ceiling, zero-read length mismatch, first-page early exit, one-pass GET,
missing-page rejection, commit rejection, byte-preserving legacy no-ops,
successful legacy migration, the legacy JSON DAO, allocation-failure rollback
and four threads performing 400 successful CAS increments. oneTBB builds repeat
CAS/migration/rollback on both memory engines.

`set_compare_commands` compares replies and subsequent stored state with the
local Redis process. It includes binary/empty/page-sized values, missing and
expired keys, four non-String kinds, GET/TTL options, conflicting and repeated
conditions, syntax errors, WATCH, source EXEC and logical DB selection. It also
checks a one-winner socket CAS race and a real primary-to-receiver source EXEC
containing successful and skipped comparisons. The replication fixture first
observes a separate readiness write, since TCP PING readiness alone does not
prove the RPC stream is registered; it does not assert initial synchronization.
NUL-option rejection and conflicting-condition order are checked separately
as intentional strictness differences.

Digest conditions IFDEQ/IFDNE and the DIGEST/DELEX family are now available;
see [streaming digests and conditional deletion](STRING_DIGEST.md). The original
value-comparison implementation does not establish complete Redis compatibility,
durable replication replay, crash safety or overall throughput superiority.
No server throughput claim is made from these correctness tests.


The final implementation passed 594/594 full-matrix CTests across the eight
C++11/C++17 memory, oneTBB, RocksDB and WiredTiger configurations. All final
sessions reached terminal success. See the
[test matrix and log identities](../tests/README.md#set-value-comparisons).
