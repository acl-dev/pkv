# Floating-point increments

`INCRBYFLOAT key increment` updates a String. `HINCRBYFLOAT key field increment`
updates one native Hash field. Both return the stored result as a RESP2 bulk
string, treat a missing target as zero, preserve existing key TTL and execute
under the same per-key lock as other mutations. Expired keys are recreated
without the expired deadline. Hash field TTL is still separate unfinished work.

The behavior is based on the Redis [INCRBYFLOAT reference](
https://redis.io/docs/latest/commands/INCRBYFLOAT/) and [HINCRBYFLOAT reference](
https://redis.io/docs/latest/commands/hincrbyfloat/), checked against the local
Redis 8.6.2 source and executable. The reference checkout reports modified=1;
these comparisons do not establish independent stock-Redis or cross-platform
compatibility.

## Parsing, calculation and replies

`common/redis_decimal` is distinct from ZSet's shortest-double score conversion.
It parses `long double` with an explicit C numeric locale, accepts decimal and
libc-supported hexadecimal/exponent notation, rejects NaN, leading/trailing
whitespace, partial parses and embedded NULs, and enforces the reference's
5120-byte buffer bound (valid input length is less than 5120). Underflow to zero
and overflow during parsing are rejected; nonzero subnormal results are accepted.

Calculation uses the platform's `long double`. Formatting uses `%.17Lf`, trims
fractional trailing zeroes and the final dot, and normalizes negative zero to
`0`. This is 17 fractional digits, not 17 significant digits or shortest-double
formatting. Very small results can round to zero when stored, just as in the
reference. Large finite results use fixed notation within the same bounded
buffer. Platform arithmetic precision can differ; ARM macOS long double and
x86 extended precision need not produce identical decimal results.

Unix formatting temporarily selects the C numeric locale on the calling thread
and restores its prior locale on success or exception. There is no process-wide
setlocale change or coroutine yield inside that scope. Windows uses the CRT's
explicit-locale parse/format APIs; Windows runtime behavior was not validated in
the local build matrix.

Error precedence differs between the two commands, matching the reference:

| Situation | INCRBYFLOAT | HINCRBYFLOAT |
| --- | --- | --- |
| Wrong type plus invalid increment | WRONGTYPE | Invalid float |
| Explicit infinite increment | Nonfinite result error | Nonfinite input error |
| Stored invalid numeric text | Invalid float | Hash value is not a float |
| Nonfinite sum | Increment would produce NaN or Infinity | Same |

Hash increment arguments are validated before opening/migrating the Hash.
String type and stored numeric value are checked before its increment argument.
Common command routing runs first in cluster mode, including on parameter
errors. Errors do not replace old values or deadlines, and a failed operation
clears the DAO output.

## Native storage and replication

Hash changes use the bounded Map AVL store. `get_bounded` reads the located
node's value length before reading its fragments; a field too long to be numeric
is rejected without loading those value pages. Valid fields need one lookup,
then the target update and normal AVL ancestor work. Unrelated value fragments
are not read or rewritten. Physical root/node/fragment formats remain unchanged.

Legacy Hashes migrate through the existing atomic page transaction. Invalid
stored numerics, overflow, allocation failure or failed commit must not publish
partial roots/pages. String increments now use the native String DAO and preserve key TTL while
replacing the bounded inline/chunk representation. Oversized native numeric
values are rejected by length without reading their data pages. Legacy String
migration retains the old owning view until commit; see `STRING_PAGES.md`.

Replication publishes final stored bytes. String increments publish their final
String root; Hash increments publish the changed native root/pages as one PKV2
transaction. Receivers do not repeat floating-point arithmetic. This avoids
additional result drift during replication, but does not provide initial sync,
replay or full Redis replication parity.

## Checks and limits

`float_increment_storage` covers numeric bounds/formatting, maximum finite and
nonzero subnormal parsing, finite-addition overflow, locale restoration, native
mdb/oneTBB behavior, key TTL, failed String reads/writes and native/legacy Hash
allocation-failure rollback. A 2001-field fixture requires a point increment to
read fewer than 80 pages, write fewer than five and load no unrelated value
fragments. An oversized numeric field must fail without loading any fragments.

`float_increment_commands` runs common cases on pkv and the optional reference:
binary/empty keys and fields, scientific/hex forms, zero normalization, expiry,
argument/type/error ordering, pipelining and eight concurrent clients. It also
compares exact replies and stored values for fixed edge cases and 200 generated
old-value/increment pairs. `page_live_stream` inspects final String and Hash
replication payloads and verifies Hash TTL plus bounded localized page edits.
Cluster fixtures cover both valid and invalid increment arguments.

These tests validate correctness and locality. They are not a whole-server
performance comparison, cross-platform arithmetic proof, field-TTL guarantee
or claim that the Redis service goal is complete.

Final local validation (2026-10-30): eight builds passed 258 CTest entries:
memory C++11 Release/C++17 ASan/UBSan 31 each, oneTBB variants 32 each, RocksDB
C++17 Debug ASan/UBSan/Release 33 each, and WiredTiger C++17 ASan/UBSan/C++11
Release 33 each. The command fixture passed 889 checks per server and 556 exact
reply/stored-value comparisons. All production changes were built before these
matrices, and no production source changed after them.

The first RocksDB Debug matrix had one 45-second socket-fixture timeout while
it ran alongside WiredTiger synchronous disk writes; its other 32 tests passed.
An isolated complete rerun, retaining the same timeout, passed all 33 in about
25 seconds (socket fixture about 11 seconds). This did not reproduce the timeout
and does not by itself prove its cause. WiredTiger's final sanitizer matrix
passed in about 117 seconds. No timeout setting was widened in this stage.
