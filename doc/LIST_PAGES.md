# Bounded List storage primitives

The `coder/list_page` component defines engine-neutral binary root/page codecs.
It is used by the native `mdb` List command DAO, migration, COPY/RENAME and PKV2
replication. See [Packed List storage](LIST_STORAGE.md) for command semantics,
evidence and unsupported-engine limitations. The format remains private.

## Bounded representation

All integers use network byte order. A legacy version-1 root is exactly 40 bytes:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | `PLR`, then version byte 1 |
| 4 | 4 | Reserved, zero |
| 8 | 8 | Millisecond expiry; all bits set means persistent |
| 16 | 8 | Logical element count, at most INT64_MAX |
| 24 | 8 | Head page identifier |
| 32 | 8 | Tail page identifier |

A zero count requires zero head/tail identifiers; nonempty roots require both
identifiers. Expiries other than persistent must be in [0, INT64_MAX]. A single
logical member may span many pages, so element count does not imply page count.

New typed List operations use a 48-byte version-2 root (`PLR`, version byte 2).
Its first 40 bytes have the same fields; bytes 40 through 47 contain a nonzero
u64 highest allocated page id. Head/tail cannot exceed that counter. The codec
still encodes/decodes version 1 when no counter is present. Typed storage promotes
old roots on an allocating mutation and never wraps an exhausted counter. See
[Packed List storage](LIST_STORAGE.md) for operations and command routing.

A page is at most 16,384 bytes, including its 32-byte header:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | `PLP`, then version byte 1 |
| 4 | 4 | Fragment count, at least one |
| 8 | 8 | Nonzero page identifier |
| 16 | 8 | Previous page identifier, or zero |
| 24 | 8 | Next page identifier, or zero |

Page links cannot reference themselves or the same nonzero neighbor on both
sides. Every fragment starts with total member bytes (u64), member offset (u64),
fragment byte length (u32) and zero reserved bits (u32), followed by those bytes.
The maximum single fragment payload is 16,328 bytes. Small members can be packed
into one page; empty/binary members are valid. Large members use fragments and
never require one large physical value. The decoder allocates only the encoded
fragment bytes, even if the declared logical member length is UINT64_MAX.
Command-level limits must be checked separately when accepting RESP input.

An unfinished fragment must be followed by the same member's exact next offset.
Completed fragments must be followed by offset zero. Only the first fragment can
continue from a previous page; only the last can continue to a following page.
Those continuations require nonzero neighbor links. Cross-page matching, link
reachability, cycles, element totals and ownership require storage-layer checks;
a codec for one page cannot establish them.

Decoding validates the entire bounded page before allocating fragments. Counts,
lengths, offsets, reserved bits, versions, truncation and trailing data are
checked. Encoding validates size before building a buffer. Failures leave output
unchanged, including injected allocation failures. Neither operation reads or
serializes other pages or allocates a buffer for the whole logical List.

## Required integration boundary

The native `mdb` engine now provides an independent physical page namespace
and atomic root/page commits through `db::transact_pages`. See
[Page transactions](PAGE_TRANSACTIONS.md) for its contract, verification and
observer guard. List commands have not been switched to that API.

Other engines still need a physical page namespace separate from arbitrary
binary logical keys. A reserved prefix in the existing logical key map
would collide with valid user input. Typed List storage now allocates ids in the root transaction; command-level
ownership transfers remain pending; this codec deliberately does not
invent an in-memory-only index or expose hidden pages through SCAN.

Native memory can publish metadata and changed pages atomically under the
existing logical key locks; List operations must now use that facility. Readers
must own stable pages across fiber yields.
Endpoint pushes/pops should touch the root and boundary pages; indexed/range
operations should traverse relevant pages. Insertion/removal may traverse the
List, but must not concatenate and reparse a whole serialized value. Large
individual members must remain fragmented in the physical store.

Preserve expiry, wrong-type/corruption handling and legacy JSON reads during
migration. Native root deletion/overwrite reclaims pages; expiry integration
remains. COPY/RENAME and cross-key moves need explicit ownership/atomicity:
copying root bytes must never
create mutable page aliases. Replica publication/application need a negotiated
atomic page transaction, not unrelated root/page SET events. Unsupported engines
must reject a mutation before committing any part. These requirements remain
open; no data migration or user-visible format switch occurs in this change.

## Verification

`list_page_codec` runs in C++11 Release and C++17 ASan/UBSan builds. It checks exact
root/page bytes, maximum-size pages, oversized-page rejection, empty/binary
members, a 1 MiB member split across bounded pages, maximum integer offsets,
invalid links/continuations, every truncation point, reserved bits, future
versions, 10,000 deterministic corruptions and all decode allocation boundaries.
These are storage-format tests, not evidence that List commands use pages yet.
