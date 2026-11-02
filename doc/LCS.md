# Longest common subsequence

LCS accepts two String keys and the LEN, IDX, MINMATCHLEN and WITHMATCHLEN
options. Missing or expired inputs act as empty strings; live non-String
inputs produce the LCS-specific type error before option validation. IDX emits
contiguous ranges in descending input order, with inclusive byte offsets and
optional match lengths. MINMATCHLEN filters ranges without changing the total
LCS length. Equal-length alternatives follow Redis's traceback tie direction.
LEN and IDX together are rejected. Inputs and results are binary-safe.

The command takes one two-key page transaction. Native roots and chunks remain
in their existing bounded String format. The existing read-only BITOP source
adapter provides native ranges and legacy codec views, including legacy range
access without migration. Both inputs are resolved at one logical time under
the same key locks. LCS changes neither stored bytes nor TTL, does not invalidate
WATCH and emits no write replication. Source EXEC reads its staged writes;
SELECT keeps the usual logical database isolation. Ordinary and transaction
cluster routing both inspect exactly two keys, excluding option arguments.

## Algorithm and resource bounds

Computation takes O(N*M) time. LEN keeps one uint32 row, choosing the shorter
input for the row dimension. Default and IDX replies keep one uint32 row and
two bits of traceback direction per input pair, instead of a full uint32
matrix. Result construction follows those directions backward. IDX constructs
ranges directly and does not allocate an unused subsequence string.

Inputs are copied into owning algorithm buffers while the snapshot locks are
held. This is transient computation memory, not a serialized stored record;
LCS does not rewrite a large JSON value or change native pages. An initial
legacy read still pays the legacy codec's parsing cost. The command can block
other operations on the locked stripes for its O(N*M) computation; this is not
a constant-time or lock-free command.

Admission retains the default Redis bound of 512 MiB for the equivalent
uint32 table, checked using root lengths before reading native payload pages.
It also rejects dimensions that cannot be represented in the uint32 algorithm.
The bound is fixed; PKV does not yet expose Redis's configurable
proto-max-bulk-len setting. A LEN query uses much less actual scratch space but
still uses the same admission rule. Algorithm allocation failure reports a
transient-memory error; allocation failure in snapshot/response construction
uses the existing request failure path. Sources remain unchanged in either
case. Replies use RESP2; RESP3 maps are not implemented.

## Verification

The storage fixture compares 800 generated binary pairs with an independent
small model in default, LEN and IDX modes, and reconstructs the subsequence
from every unfiltered IDX range list. Allocation ceilings distinguish linear
LEN memory and compact traceback from a full uint32 matrix. Legacy/native
70-KiB inputs, WATCH preservation, page-read failure and oversized admission
before page reads are checked. Concurrent atomic MSET pairs verify that LCS
never observes a mixed input snapshot. Allocation budgets cover both input
formats and all three result paths, including successful control budgets.

The socket fixture compares all options, binary/empty keys and values,
ambiguous subsequences, errors and their precedence, expiry, logical databases
and staged EXEC against an isolated local Redis reference. It separately checks
ordinary/queued cross-slot rejection and same-slot execution. An initial
cross-slot regression exposed the ordinary String route's missing second-key
classification; both ordinary and queued routes now include LCS.

A binary option containing NUL is rejected instead of accepting a truncated
option as the local modified=1 Redis 8.6.2 reference does. This deliberate
strict-parsing difference is tested separately. The reference is not an
unmodified upstream certification. The command contract follows the official
[LCS documentation](https://redis.io/docs/latest/commands/lcs/).

The final eight-configuration regression passed 518/518 CTests, including
1,112 LCS differential assertions per configuration. See the
[validation matrix](../tests/README.md#lcs) for logs, standards and backends.
