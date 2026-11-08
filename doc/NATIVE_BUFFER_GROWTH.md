# Native replacement buffer growth

MDB synchronous commits prepare replacement strings before publication. Two
worker-local buffers can be reused under a scoped busy lease; nested or live
preparations use independent storage. Publication swaps prepared strings into
the database only after all preparation succeeds. The old stored strings can
then return to the bounded workspace.

An assignment that grows a reused string can geometrically increase capacity.
That capacity becomes persistent database storage after the swap. For example,
the macOS probe stored 8240-byte pages with capacity 12383, increasing the
retained footprint even though encoded pages were small enough.

When a replacement is at least one eighth of the 16384-byte page bound and
outgrows the available buffer, preparation now copies it into a fresh string
and swaps that string into the preparation slot. Smaller incremental updates
keep amortized growth. Buffers that already fit remain reusable. This policy
does not shrink every stored string or establish an allocator/RSS bound;
string padding, allocator size classes and existing cached capacity remain.

The source snapshot and stored target are unchanged if the fresh copy throws.
No database string is modified until normal publication. Existing workspace
lifetime, nested preparation, cancellation and retained-capacity limits remain
in force. Replica snapshots continue to receive the original staged bytes.

`native_commit_buffers` tests a sequence of growing page replacements and
checks stored capacity against a fresh-copy reference, as well as complete
stored contents. Other tests cover allocation rollback, prepared lifetimes,
transactions, observers and replication.

The macOS probe separately records encoded bytes, string capacity,
`malloc_size` and process RSS. These quantities must not be treated as
interchangeable. The
[performance report](benchmarks/performance-exact-replacement-2026-11-03.json)
includes the raw probes, network comparisons and remaining performance gap.

Synchronous updates of one existing root and one existing page now locate and
lock their shard once. Both replacement strings are prepared before either
stored string is swapped. Missing entries, page clearing, other action shapes,
callback preparation and owned plans use the general commit implementation.
The shared workspace stage method applies the same large-value growth policy
to both implementations. Allocation failures at either copy preserve both
stored values and all staged inputs. This path does not change root indexes,
page ownership, observer input lifetimes, WATCH, or replication formats.

Bulk synchronous commits can reuse up to 64 additional string buffers from the
same worker-local lease. Cached character capacity is limited to 1024 bytes per
buffer (64KiB total), plus string-vector metadata. Nested actors and owned plans
use independent copies. Target pointers stay in the ordinary transient commit
vector; the cache contains strings only. The original vector reservation and
commit object fields are preserved. Warm bulk overwrites still allocate their
metadata vector once; this is not an allocation-free command path.

After publication or rollback, commit_state returns eligible strings before its
inline workspace releases the lease. Inputs remain owned by the caller for
observers and prepared plans. Oversized scratch capacity (more than incoming
size plus one quarter and 32 bytes) is replaced by a fresh copy; growth also
uses a fresh copy. Values with capacity above 1024 are not retained in the
spill cache. These are idle-cache bounds, not live transaction or RSS limits.
