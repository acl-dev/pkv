# Linear KILL capture and safe client metadata

CLIENT KILL now requests an unordered registry snapshot. Membership capture is
O(N) with O(N) temporary references, without sorting IDs that the command never
uses. ID-filtered KILL still uses one registry bucket. Ordered snapshots remain
available for callers that need stable traversal; existing callers retain the
ordered default. MAXAGE's command clock is read only when MAXAGE was supplied.
Neither change weakens target validation, reply preparation or descriptor
retirement guarantees.

## Copied metadata

The connection registry now associates a public handler's existing client state
with its record. A copied view contains ID, protocol, connection name, library
name and library version. This is internal infrastructure for CLIENT INFO/LIST;
CLIENT INFO/LIST are now implemented by [the runtime extension](CLIENT_INFO.md).
Queue/buffer/command observations use real structures and published gauges.

Mutable metadata has one connection-local mutex. Setters validate and prepare
owned strings before taking the lock, then commit with swap. HELLO commits its
name, protocol and successful authentication together. RESET changes protocol
and name under the same lock. A reader consequently cannot combine the new
HELLO name with the old protocol. Library attributes remain unchanged by RESET.
Ordinary reference getters and login state are still owned by the connection's
actor; other threads must request a copied view. This does not turn arbitrary
reference access into a thread-safe API.

A metadata reader first holds the record mutex, then copies under the client
state mutex. Retirement holds the record mutex and clears the borrowed pointer
before the handler destroys client state. Retained registry records cannot read
a destroyed handler. No shared record retains ownership of the full handler or
socket. A missing/retired association returns false without changing the output.
Copying stages a complete local view before a nonthrowing move into the output;
an allocation failure leaves the caller's previous view intact and releases the
locks. The record/state lock order has no inverse path: metadata writers take
only the state lock, membership capture takes bucket locks, and retirement takes
bucket then record. No lock spans response serialization or network IO.

There is no new metadata lock or copy on GET or other ordinary KV execution.
The mutex increases per-connection state size, and view copying costs memory
proportional to the metadata strings. Client counters, DB selection and parser
resource gauges are integrated by [the runtime extension](CLIENT_INFO.md);
the earlier validation below describes the snapshot-only revision.

## Verification

`client_snapshot` checks ordered/unordered membership equality and failed
unordered-capture allocation. It injects failure at each of three large-string
copy allocations, requiring the previous output to survive. Four readers race
10,000 HELLO pairs and RESET updates and reject mixed name/protocol versions.
A retained record races metadata reads with retirement and owner destruction,
then must reject further reads. HELLO allocation failure preserves the old
protocol/name/login state. The existing registry descriptor-reuse, KILL,
HELLO/RESP3, AUTH and transaction fixtures remain enabled.

Standalone TSan builds instrument both the new copied-view fixture and the
existing registry fixture. These cover standard-library metadata/lifecycle
code, not the whole ACL server:

```sh
clang++ -std=c++17 -g -O1 -fsanitize=thread \
  -fno-omit-frame-pointer -pthread -I . \
  tests/client_snapshot_test.cpp common/client_registry.cpp \
  common/client_state.cpp -o /tmp/pkv-client-snapshot-tsan
/tmp/pkv-client-snapshot-tsan
```

The optional `client_snapshot_bench` target compares ordered and unordered
capture in the same binary for 1,024 through 65,536 records. Three warmups per
mode precede 21 alternating-order rounds. It excludes record construction,
network shutdown, metadata string copying and result destruction from the timed
interval. It is a registry CPU microbenchmark, not end-to-end KILL throughput.
Full test results, source hashes and measurement limits are retained in the
[validation report](benchmarks/client-snapshot-validation-2026-10-31.json).


## Measured results

All eight configurations passed 734/734 CTests: memory/C++11 memory 82 each,
oneTBB/C++11 oneTBB 91 each, RocksDB Debug/Release 97 each and WiredTiger
Debug/C++11 97 each. WT suites ran separately. Both standalone TSan fixtures
passed. Source/binary/log hashes are in the validation report above.

The same-binary C++11 Release capture microbenchmark observed:

| Records | Sorted median ms | Unsorted median ms | Time ratio |
| ---: | ---: | ---: | ---: |
| 1,024 | 0.027625 | 0.005583 | 4.95 |
| 4,096 | 0.158709 | 0.021583 | 7.35 |
| 16,384 | 0.553541 | 0.078542 | 7.05 |
| 65,536 | 2.177292 | 0.288709 | 7.54 |

The ratio is sorted time divided by unordered time. These are synthetic,
single-thread medians; they do not include metadata copying or connection
shutdown and do not establish end-to-end KILL throughput. Per-round values
were not retained. See [measurement provenance](
benchmarks/client-snapshot-capture-2026-10-31.json). Run with:

```sh
cmake --build /tmp/pkv-c11-build --target client_snapshot_bench
/tmp/pkv-c11-build/tests/client_snapshot_bench
```

A separate revision comparison checked all 37,748,736 GET replies using identical
C++11 Release flags, two memory-server threads, four clients, 64-byte values,
256-command pipelines and disabled active expiry. Three rotating rounds gave
median rates of 1,511,932/s for baseline RESP2,
1,527,869/s for current RESP2 (+1.05%) and
1,528,823/s for current RESP3 (+1.12%). These
small observed differences do not establish causation, statistical significance
or a broad performance advantage. The workload does not exercise live metadata
copying, connection churn or other backends. See the
[checked GET report](benchmarks/client-snapshot-get-2026-10-31.json).

```sh
python3 tools/benchmark_resp3_get.py \
  --baseline /tmp/pkv-kill-measured-c11-pkv \
  --pkv /tmp/pkv-snapshot-measured-c11-pkv \
  --baseline-flags /tmp/pkv-kill-c11-flags.make \
  --new-flags /tmp/pkv-snapshot-c11-flags.make \
  --batches 4096 \
  --output doc/benchmarks/client-snapshot-get-2026-10-31.json
```

On this native 64-bit host, sizeof(client_state) is 152 bytes,
sizeof(client_connection) is 152 bytes and sizeof(client_snapshot) is 88 bytes.
These exclude heap strings, handler/parser buffers and allocator overhead; they
are not whole-connection memory totals or portable ABI guarantees.
