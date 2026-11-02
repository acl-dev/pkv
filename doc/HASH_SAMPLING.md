# Native Hash random-field sampling

`HRANDFIELD key [count [WITHVALUES]]` uses `dao::paged_hash` on native memory
and single-instance disk backends. The no-count form returns one field or a nil
bulk string. With count it returns an array: nonnegative counts select distinct
fields, clamped to HLEN; negative counts allow repeats and return the requested
absolute count for a nonempty Hash. WITHVALUES emits flat field/value pairs in
RESP2. Zero and missing/expired Hashes produce empty arrays with count.
These semantics follow the [Redis command reference](
https://redis.io/docs/latest/commands/hrandfield/).

The AVL subtree counts select uniform numeric ranks. A sparse Fisher-Yates
sampler tracks only ranks changed by the requested sample. Hash, Set and ZSet
share this implementation. Small samples require O(k) scratch entries and
O(k log n) tree work, plus returned field/value bytes. Selecting all distinct
fields uses one traversal; its order is not randomized. Repeated samples use
independent uniform ranks and no swap table entries. Sampling is not
cryptographically secure.

The rank lookup optionally reads the value from the selected node, avoiding a
second field search for WITHVALUES. Without WITHVALUES it never reads the value
fragments. No whole Hash or complete member-pointer array is materialized for a
small sample. The required response still owns its returned fields/values.
Existing physical roots/nodes/fragments and their 16-KiB page bounds are unchanged.

Sampling holds the same per-key transaction lock as HSET/HDEL and reads one
coherent field/value snapshot. Native sampling changes no root/pages or key TTL.
Legacy JSON Hashes migrate on first DAO access under the existing transaction;
failure leaves their old root and page family intact. A successful sampling
read can therefore commit migration or expiry cleanup. Hash field TTL is still
unimplemented and is not implied by key-TTL preservation.

Parsing, wrong-type precedence, arity, count bounds and routing are covered by
the command tests. Cluster routing runs at the common Hash dispatch entry;
unavailable slots take precedence over invalid count/options, matching the
reference behavior. WITHVALUES uses the same C-string option comparison as the
local Redis reference, including acceptance of a NUL suffix; field and value
payloads always retain their complete byte lengths. Unrepresentable response
sizes fail before sampling/allocation. Resource exhaustion of smaller valid
requests remains subject to the server's broader memory-limit work.

`hash_sampling` runs the same deterministic assertions on pkv and the optional
local Redis reference build, including binary/empty fields, large values,
positive/negative counts, key TTL, errors, pipelines and concurrent full-Hash
writes versus sampled snapshots. `paged_hash_dao` adds allocation-failure
migration rollback, malformed/future-record preservation, native mdb/oneTBB
sampling, fixed-seed rank coverage and a 1024-field locality fixture. Sampling
three fields there must read fewer than 150 pages; only WITHVALUES may read the
six fragments holding the three selected 20000-byte values.

This provides correctness and page-locality evidence. It does not establish a
whole-server throughput advantage over Redis. HINCRBYFLOAT is now implemented; see `FLOAT_INCREMENTS.md`. Field expiry and
newer combined Hash commands remain separate compatibility work.

Validation (2026-10-30): all eight full build matrices passed 242 CTest entries
(memory C++11/C++17 29 each, oneTBB 30 each, RocksDB C++17 Debug/Release 31 each,
WiredTiger C++17 Debug/C++11 Release 31 each). The new socket fixture passed
263 common checks per server against pkv and the local modified Redis 8.6.2
reference, plus two pkv-only impossible-size cases. After the final cluster
error-precedence correction, every build was rebuilt and its sampling/routing
fixtures rerun: 16 further passing entries. No storage/sampler code changed
after the full matrices. New code respects the 80-column and 200-line function limits. No general throughput or complete Redis claim is made.
