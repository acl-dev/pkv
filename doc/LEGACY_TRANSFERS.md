# Bounded legacy COPY and RENAME

COPY, RENAME and RENAMENX now import enumerable old-format values directly into
the destination's native page transaction. String, List, Hash, Set and Sorted
Set use the shared native importer. New roots and individual pages remain at
most 16384 bytes, including when a member exceeds that size. The logical
payload and exact absolute expiry are retained; no replacement whole-container
JSON value is written on this path.

COPY leaves the source bytes and page family unchanged, including its WATCH
state. RENAME stages source deletion together with the imported destination.
Destination initialization drops its entire previous page family, including
unused old pages. Native-to-native transfers keep their existing typed clone
paths. RENAME still traverses/copies pages because physical ownership includes
the logical key; it is not a constant-time metadata swap.

## Codec extension contract

record_view::native_importable() defaults to false. Opaque and read-only views
therefore retain their byte-preserving transfer behavior. A view may opt in
when it exposes a complete native-compatible payload through the existing
String range, List range or Hash/Set/Sorted Set size/visitor accessors. The
importer validates traversal, duplicates, limits and typed-store errors; the
capability does not bypass validation. No legacy encoder is required.

Default yyjson views opt in for normal Redis values. Old empty Hash records
keep their compatible opaque path because native Hash trees require members.
A custom view can opt in with streaming visitors while refusing full owning
collection materialization. Native metadata views use their typed clone path,
not this legacy-view capability.

## Eligibility and failure boundaries

Native import requires a page-capable source and a destination whose engine and
registered observers support page writes. Nonpage engines, legacy observers
and nonenumerable views retain the old compatibility path. These paths can
still preserve large old serialized values; they are not native migration.

The capability check is a hint. Commit rechecks observer support and complete
batch byte/record budgets. An unsupported native commit or a storage failure
returns an error without changing either key. It must not retry as a large
opaque write merely because a page batch exceeded a limit. A source-format change retries under the key locks and rechecks the current
capability on every attempt. A view that becomes importable again is not forced
onto the opaque path. A native root never moves alone without its pages.

Conditional COPY/RENAMENX and same-key RENAME do not migrate values when no
logical transfer is needed. Source and destination existence/expiry are checked
under their key locks. The source decoded view remains alive while checking an
expired destination, so destination decoding cannot replace the import input.
A source removed between probes or during a retry reports no such key for both
RENAME forms; an old successful decode cannot leave a stale storage-error status.
Unknown/future metadata and malformed legacy payloads are preserved on errors.

## EXEC and replication

Source EXEC admission now decodes transfer-source metadata and checks this
capability instead of declining all old JSON roots. Repeated source names are
sorted/deduplicated before preflight. Eligible one-database, single-domain EXEC
stages COPY/RENAME with later commands and commits once. Opaque sources and
incompatible observers retain the previous path. Cross-database coordination
and multiple-store atomic RENAME remain separate work.

Observers receive the existing bounded root/page batch with logical DB identity.
COPY's unchanged legacy source root is excluded from captured wire updates.
RENAME includes source deletion and destination replacement in the same batch.
Normal replication and whole-EXEC capture reuse those batches. Publication is
not durably coupled to source commit; reconnect replay remains unfinished.

## Verification

legacy_transfer checks all five kinds with 70-KiB payloads, binary/empty members,
exact TTL, conditional/same/missing keys, WATCH, old orphan-page cleanup, rejected
storage/unsupported commits, allocation failures, nonpage/legacy-observer
compatibility and custom visitor-only views. Captured batches replay into a
second database and retain the expected source WATCH behavior. Backend variants
exercise oneTBB and separate-process RocksDB/WiredTiger reopening.

Actual-handler source_exec checks one source commit for legacy COPY/RENAME plus
other commands, failed final commit and a complete source allocation sweep.
These checks do not establish cross-database atomicity, power-loss guarantees,
Redis replication parity or an overall performance advantage over Redis.


The memory DAO sweep uses 962 allocation-budget positions across five types and
two operations. The actual EXEC sweep uses another 237 positions; both include
successful control budgets. Exact final matrix counts and logs are in
[the test record](../tests/README.md).


The pre-correction full matrix passed 454 CTests. Later deterministic source
lookup regressions exposed an oversized opaque fallback and stale RENAME missing
status. Both corrections rebuilt all eight configurations and passed 88 affected
checks, including the new cases; the full matrix was not repeated afterward.
One earlier WT Debug full run hit a concurrent I/O timeout; its unchanged
standalone full retry passed. See the test record for exact logs and limits.
