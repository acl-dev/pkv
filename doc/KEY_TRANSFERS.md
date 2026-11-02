# Key transfers

RENAME and RENAMENX require an existing unexpired source. RENAME overwrites the
destination and returns OK; RENAMENX returns 0 for a live destination and 1 after
a successful move. Types need not agree. Equal source/destination names produce
OK/0 without writing; a missing source returns ERR no such key. Empty/binary
key names are supported.

COPY accepts DB and REPLACE options. It returns 1 after copying, otherwise 0
for a missing source or blocked destination. Equal source/destination in the
same logical database is an error. SELECT and COPY use separate logical DB
instances; databases defaults to 16, while db_count controls physical shards.
See [logical databases](LOGICAL_DATABASES.md) for remaining MOVE/flush limits.

## Layout and atomicity

Native values clone typed pages into an independent destination family while
preserving exact expiry. Native String/List/Hash/Set/Sorted Set roots never move
alone without their owned pages. Enumerable legacy views now import directly
into bounded destination pages; see [legacy migration](LEGACY_TRANSFERS.md).
Only opaque/nonpage compatibility paths preserve the exact old serialized bytes.
Custom views opt in through native_importable; read-only extensions keep their
previous byte-preserving behavior by default.

Both key stripes are locked in common sorted order. RENAME commits destination
replacement and source deletion together or returns an unsupported/error result
without changing either. COPY keeps source bytes/pages/WATCH unchanged and
commits only the destination. Overwriting reclaims its old family and TTL.
A native commit rejection never falls back to an opaque large-value write.

Multiple independent physical stores still cannot atomically RENAME keys across
their commit domains. There is no sequential source-delete/destination-write
fallback. Same-key/blocked conditional cases need no storage commit. Native
RENAME still copies payload pages because their physical owner includes the
logical key; constant-time ownership transfer remains optimization work.

## Validation, observers and EXEC

Legacy payloads are validated before transfer. JSON Hash checks include field
types and duplicate binary names without copying field values. Import validates
member traversal and typed limits. Native payload pages are validated as they
are traversed; same-key metadata-only operations do not scan every payload page.
Future/invalid metadata and invalid legacy sources return errors without moving
their bytes. Expired physical cleanup is deferred on nonmoving/error paths.

Unconditional RENAME replaces the target without decoding its old payload.
Conditional operations decode a physically present target to distinguish live
and expired keys; invalid/future target metadata is preserved with an error.
The source view stays alive during that check. Lookup/commit errors clear the
reported success flag and retain source/destination.

Native observers receive one bounded page batch, including both RENAME effects.
COPY's unchanged source is omitted from captured updates. PKV3 whole-EXEC capture
and ordered destination apply reuse these batches. Eligible source EXEC groups
also stage legacy transfers and commit once. Cross-logical-DB/physical-store and
opaque compatibility fallbacks still lack coordinated recovery. Source commit
and publication are not durably coupled; initial sync/reconnect replay remain
unfinished. See [source EXEC](SOURCE_EXEC.md) and [replication](REPLICATION.md).

## Verification

Existing socket coverage includes 2,696 shared key-transfer assertions per
endpoint: all five kinds, TTL, overwrites, same/missing/expired keys, NX,
binary/empty names, arity, model operations and concurrent rename readers/races.
Routing adds six cases. Native typed-store tests cover cloning and failure
preservation. The legacy_transfer fixture adds large migration, old page-family
cleanup, WATCH, batch replay, codec capability, allocation/commit failure and
independent disk reopening. Exact current matrix results are recorded in
[the test record](../tests/README.md).
