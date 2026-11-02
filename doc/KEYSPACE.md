# Full keyspace matching

KEYS pattern returns each matching live key once in the selected logical DB.
Patterns use the same binary-safe glob matcher as SCAN, including NUL, arbitrary
bytes, escapes and bracket classes. Reply order is unspecified. Empty keys and
empty patterns are valid. Values of every supported type are eligible; private
native pages are not user keys. The command follows the official
[KEYS contract](https://redis.io/docs/latest/commands/keys/).

A KEYS request takes exclusive execution admission before entering its handler.
The complete traversal therefore observes one write boundary and logical time.
Ordinary SCAN retains its separate per-page behavior. An EXEC already owns this
admission; KEYS reads the staged workspace and deduplicates overwritten source
keys that its scan overlay can emit more than once. Staged deletes are absent,
and new staged keys are visible. The pattern has no key-routing dependency.
In cluster mode KEYS enumerates the node's local DB instead of routing the
pattern or requiring that every matching key share a hash slot.

The implementation uses the existing memory/TBB/RocksDB/WiredTiger root scan
indexes and collects matching keys in an owning hash set. Matching live values
are not migrated or rewritten. Expired matching roots may be removed through
the normal atomic deletion path, reclaiming their private pages. Corrupt or
unsupported matching records and backend errors return errors rather than
partial result arrays. Allocation failure clears the DAO output; request-level
failure uses the existing connection/response handling. A backend cursor that
fails to advance is rejected instead of looping forever.

## Cost and scope

With bounded key/pattern lengths, traversal and expected hash-set work are
linear in the database key count. Matching long adversarial patterns can cost
more. Memory grows with the matched key names, one candidate scan page and the
normal RESP reply. COUNT remains a backend hint, so collision-heavy buckets
may exceed the internal page target. There is no streaming reply or strict
latency/memory bound. KEYS holds the shared node's execution admission for the
whole traversal, so it can delay commands in other logical DBs. Use SCAN for
incremental enumeration. The optional Redis cluster hash-tag pattern shortcut
is not implemented; local results are still complete.

No new stored-value format is introduced. Native metadata checks read only
roots; no List/Hash/Set/ZSet or large String payload pages are loaded. Legacy
codecs still parse their old record once when its metadata is needed, retaining
the cost of old large JSON records. Backends lacking root scans report the
existing unsupported-scan error. [DBSIZE](DBSIZE.md) now reports stored-root
counts separately. [Active expiry](ACTIVE_EXPIRY.md) now reclaims expired roots
in the background. [RANDOMKEY and TIME](KEY_SAMPLING.md) are now implemented. Further
keyspace administration remains open.

## Shared metadata inspection

`record_codec::stored_header` reuses the same native-root validation as full
`decode_stored` views. SCAN/KEYS type/expiry filtering and WATCH now use this
metadata-only path, removing an otherwise unused `paged_record_view`
allocation for each native root. Existing custom legacy codecs need no new
virtual method: they retain their owning decode and expose its header.
Non-inline String and structured roots require no C++ heap allocation for
this inspection. Inline String headers now use the same heap-free metadata inspection;
only full payload reads copy their bytes. Root reads, key collection and watch subscriptions
have their own allocations; this is not an allocation-free command claim.

The storage fixture prohibits C++ allocations and checks all five native types
against the owning-view path, which correctly reports allocation failure under
that budget. Byte mutations compare validation status and metadata between
both APIs. Further tests cover 600 keys, binary/empty patterns, a 70-KiB
legacy/native String, unchanged live WATCH, workspace overwrite/delete/add,
concurrent atomic key transfers, backend errors and nonadvancing cursors. A
page-read spy rejects every payload-page lookup to verify metadata-only scans.
The C++ allocation sweep covers 1,249 positions, including its successful
control. The socket suite compares 336 assertions against an isolated local
Redis 8.6.2 modified=1 reference, plus node-local cluster/EXEC checks. Reference
comparisons do not certify compatibility with unmodified upstream Redis.

The feature passed 534/534 full-matrix CTests and eight final formatting-path
checks. See the [validation matrix](../tests/README.md#keys-and-native-metadata-inspection)
for build coverage, exact verification order and logs.
