# Legacy expiry migration

EXPIRE, PEXPIRE, EXPIREAT, PEXPIREAT and PERSIST share the bounded native importer
used by String, List, Hash, Set and Sorted Set DAOs. A live legacy key is imported
and its expiry changed within one page transaction. The old root and page
family are replaced together; every new stored root/page is at most 16384 bytes.
Large individual members are fragmented by the existing typed stores. No new
whole-container serialization is introduced.

The importer owns no commit. Typed DAO calls and generic expiry calls supply
their existing page transaction, decoded view and error status. Lists import a
bounded member batch, Strings read ranges, and Hash/Set/Sorted Set visit members.
The legacy codec still owns the old document during migration; this removes
large stored replacements, not the memory cost of parsing the old input.

## Conditions, retries and publication

The metadata probe checks logical existence and EXPIRE NX/XX/GT/LT or PERSIST
eligibility first. A rejected condition or persistent-key PERSIST leaves bytes,
pages and WATCH unchanged. Immediate expiration deletes directly without
importing the payload. Already-native keys keep their metadata-only fast path,
without reading or rewriting payload pages.

A legacy mutation starts a page transaction after releasing the probe lock.
It decodes the current root and rechecks conditions under that transaction's
key lock; another actor may have changed type, expiry, encoding or existence
between those two operations. Migration and the new expiry either commit
together or expose neither. An unsupported engine or observer commits nothing
and retries the current value through the old legacy path. Storage failure,
invalid input and allocation failure do not trigger that fallback.

A successful expiry change is a logical mutation, including when it imports
legacy bytes. Its root is not marked maintenance-only, so source WATCH is
invalidated after commit. Observers receive one ordered root/page batch, with
the existing logical DB identity, reset flag and bounded payloads. Existing
native replication support transports that batch without flattening the value.

## EXEC and current limits

Legacy expiry keys no longer decline source workspace eligibility. Their
migration is staged along with later EXEC commands and committed once on
supported single-domain engines. A final storage failure leaves the original
legacy bytes, WATCH and subsequent staged writes unchanged. Migration remains
subject to workspace byte/record limits and existing type/member limits.

Legacy COPY/RENAME still use their compatible whole-value path and may cause
EXEC fallback. Nonpage engines/observers may still rewrite legacy values.
Cross-database commits, durable publication and reconnect replay remain open.
No automatic startup rewrite or background migration is installed.

## Verification

The legacy_expiry fixture checks all five types with 70-KiB payloads, binary
and empty members, conditional no-ops, PERSIST, WATCH, rejected storage commits,
observer batch bounds and allocation-failure sweeps. Optional backend fixtures
exercise oneTBB, RocksDB and WiredTiger; disk fixtures reopen the final migrated
values in a separate process. Actual-handler source_exec checks one source
commit and failed final commit for a large legacy expiry inside EXEC.


The full eight-configuration matrix passed 440 CTests, followed by eight
rebuilt actual-handler checks after a formatting-only planner line wrap. See
[the test record](../tests/README.md) for counts, logs and limits. These runs
cover process reopening and source commit boundaries, not arbitrary power loss.
