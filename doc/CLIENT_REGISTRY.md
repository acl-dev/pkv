# Connection registry foundation

Public Redis connections now register their existing CLIENT ID with a
service-owned registry. The connection handler retires that record before the
ACL stream owner closes its descriptor, including exception unwinding. A saved
snapshot retains the record, not ownership of the socket or the handler.

This is infrastructure for CLIENT INFO/LIST/KILL and later cancellation work.
The initial registry revision did not advertise those commands. CLIENT KILL
is now implemented as described in [its scope](CLIENT_KILL.md); INFO/LIST remain
unimplemented. The
companion replication protocol is not registered as a public Redis client.
Authentication, client metadata and database selection keep their current
connection-owned behavior; the registry does not expose unsynchronized pointers
to those fields.

## Lifetime and concurrency

Membership uses 64 mutex-protected hash tables. Adding, finding and retiring a
connection use one bucket. Snapshotting acquires all buckets in a fixed order,
copies shared record references and releases the locks. Ordered callers then
sort IDs; KILL uses linear unordered capture. Membership
is captured together; individual connection states can change after the locks
release. Normal key commands do not acquire a registry lock.

Each record has a separate mutex for its non-owning descriptor. A stop request
and retirement use that same mutex. Retirement invalidates the descriptor and
marks the record retired before the owning handler returns to ACL. A stale
snapshot consequently cannot shut down a new connection that reuses the old
descriptor number. Record destruction never closes a socket.

Immediate stop requests call shutdown and wake blocked socket IO. The handler
also checks the atomic stop state before each parsed pipeline command. An
already executing command is allowed to finish; this mechanism does not roll
back a committed operation or interrupt an EXEC in its middle. Deferred stop is
reserved for requests made by the connection's own command handler: it permits
pending reply bytes to be written and then closes through the existing owner.
It is not an asynchronous wakeup for an idle connection. A later immediate stop
can escalate a deferred one without counting another removal.

Lock order is bucket then record. Stop requests take only the record lock, and
snapshots take only bucket locks. No registry bucket lock spans socket IO or a
command handler. The service must outlive its handlers, as in the existing
server lifecycle. Registry destruction invalidates retained records and does
not assume ownership of descriptors. A probe against the installed ACL library
also verifies that read/write failures retain the descriptor until owner close;
the read/write source paths only set error state.

## Failure behavior and verification

An invalid descriptor or nonpositive ID is rejected before insertion. Duplicate
IDs cannot replace existing records. Failed allocation during construction or
insertion publishes no entry. Failed snapshot allocation releases every bucket
lock without changing membership. Retirement checks both ID and record identity,
so stale or foreign records cannot remove a replacement.

`client_registry_test` exercises allocation failure, same-bucket collisions,
foreign ownership, retained snapshots, eight concurrent membership workers,
500 forced descriptor-reuse cases and 300 stop/retire/reuse races. It also checks
that deferred shutdown preserves the final reply and immediate shutdown does
not close the owner's descriptor. `client_lifecycle_test` uses real ACL streams
and handlers to verify registration, stopped-pipeline rejection, database
preservation, retirement ordering and independent connection lifetimes.

Validation results are recorded in
[the validation report](benchmarks/client-registry-validation-2026-10-31.json).
These checks establish lifecycle behavior; they do not establish a throughput
advantage over Redis or complete Redis client administration.

The standalone registry race check needs only the standard library and native
sockets, so it can be instrumented separately from ACL:

```sh
clang++ -std=c++17 -g -O1 -fsanitize=thread \
  -fno-omit-frame-pointer -pthread -I . \
  tests/client_registry_test.cpp common/client_registry.cpp \
  common/client_state.cpp -o /tmp/pkv-client-registry-tsan
/tmp/pkv-client-registry-tsan
```


## Local GET cost observation

The retained pre-registry authentication build and this change use equal C++11
Release compiler flags, the memory backend, two server threads, four clients,
64-byte values and 256-command pipelines. Active expiry is disabled. Three
rounds rotate baseline RESP2/current RESP2/current RESP3 order. All 37,748,736
measured response frames matched their expected bytes.

Median rates were 1,497,762/s for baseline RESP2,
1,463,221/s for current RESP2 (-2.31%) and
1,462,269/s for current RESP3 (-2.37%). This
run observes a small decrease; it does not establish its cause or statistical
significance. The new handler checks an atomic stop state before each command,
which preserves the boundary at which an external stop takes effect. This is
not a speedup claim or a comparison with stock Redis. Connection churn, stop
latency, large client snapshots, other backends and long-running mixed loads
are outside this measurement.

The [raw benchmark report](benchmarks/client-registry-get-2026-10-31.json)
contains per-sample timings and binary/compiler/script hashes. Reproduce with:

```sh
python3 tools/benchmark_resp3_get.py \
  --baseline /tmp/pkv-auth-measured-c11-pkv \
  --pkv /tmp/pkv-registry-measured-c11-pkv \
  --baseline-flags /tmp/pkv-auth-baseline-c11-flags.make \
  --new-flags /tmp/pkv-registry-c11-flags.make \
  --batches 4096 \
  --output doc/benchmarks/client-registry-get-2026-10-31.json
```

The baseline binary matches the new binary hash retained in the earlier
[authentication benchmark](benchmarks/auth-get-performance-2026-10-31.json).
Full-matrix validation passed 710/710 CTests: memory 79, C++11 memory 79,
oneTBB 88, oneTBB C++11 88, RocksDB Debug/Release 94 each, and WiredTiger
Debug/C++11 94 each. WT suites ran separately. The independent TSan and installed
ACL read/write-error probes also passed. These checks leave the broader Redis
compatibility goal open.


The later [copied metadata extension](CLIENT_SNAPSHOTS.md) associates state with
records and detaches it before owner destruction. It also removes sorting and
unneeded clock reads from KILL. Earlier validation counts describe the retained
registry-only revision.
