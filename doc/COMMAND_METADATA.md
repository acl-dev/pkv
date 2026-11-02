# Command discovery and key arguments

PKV implements COMMAND, COMMAND COUNT, COMMAND INFO, COMMAND LIST,
COMMAND GETKEYS, COMMAND GETKEYSANDFLAGS, COMMAND HELP and basic COMMAND DOCS.
Replies use RESP2. Discovery is filtered through the actual handler registry;
COUNT counts top-level commands, while LIST also includes known subcommands.
Unknown INFO names return null bulk strings. INFO without names returns the
registered top-level commands. Names and pattern filters are case-insensitive.
Binary key names and duplicate key positions are preserved by key extraction.

INFO contains the ten Redis metadata fields, including key specifications and
supported subcommands. LIST supports FILTERBY PATTERN, ACLCAT and MODULE;
MODULE returns an empty list because PKV does not load Redis modules. ACL
categories are discovery labels; they do not implement authorization. DOCS
currently supplies only a basic summary and group. Full argument descriptions,
version history and complexity documentation remain open. Metadata is not a
claim that every Redis option of a registered command has been implemented.

GETKEYS and GETKEYSANDFLAGS inspect arguments without executing the target.
Index/range and counted-key specifications cover the registered key commands;
SET and BITFIELD report their argument-dependent access flags. The outer
COMMAND query has no database key dependency, including when queued in EXEC.
Inspecting SELECT does not select a database, and inspecting a write does not
perform that write. Cluster routing must not route the inspected keys as if
COMMAND itself accessed them.

## Static catalog

`common/command_catalog_data.inc` contains constant RESP metadata and compact
key rules. Runtime discovery does not require Redis, JSON parsing or a new
serialization dependency. Catalog lookup compares names without allocating a
lowercase copy, including for large unknown names. Responses are still built
in the normal owning RESP object tree, so output size and allocation cost grow
with the requested metadata.

The optional generator uses an explicitly selected local Redis binary:

```sh
python3 tools/generate_command_catalog.py \
  --redis-server /absolute/path/to/redis-server
```

It starts an isolated reference server, reads metadata for the registered PKV
commands, prunes unsupported subcommands and sorts the results. It preserves
RESP simple/bulk string distinctions, removes optional key-spec prose and emits
only a basic self-authored DOCS summary and the reference group. Custom cluster
subcommands have local descriptors. The generated header records the reference
binary digest. Generation rejects unsupported key-spec forms rather than
silently guessing. The registry consistency fixture catches missing catalog
entries and changed minimum arities. Adding a command still requires reviewing
its complete descriptor and key behavior. Two consecutive generations from the
same reference produced identical bytes.

The reference used here is a local Redis 8.6.2 build reporting modified=1.
This comparison does not certify compatibility with unmodified upstream Redis.
The schema follows the official [COMMAND documentation](https://redis.io/docs/latest/commands/command/),
[INFO documentation](https://redis.io/docs/latest/commands/command-info/) and
[key extraction documentation](https://redis.io/docs/latest/commands/command-getkeys/).

## Allocation failure ownership

The new metadata-in-EXEC allocation sweep exposed a RESP parser ownership bug:
a completed object could enter the output vector before acquisition of its
replacement failed. Both owners then returned the same object to the cache.
The parser now acquires the replacement before publishing the completed object
and releases it if publication fails. Root/child builders also release objects
when insertion into their owning vector fails. The vector cache reserves its
bounded capacity at construction, so object return during destruction does not
allocate. A parser interrupted by allocation failure must be discarded/reset;
resuming the same partial parse is not promised.

`resp_alloc_*` checks every allocation budget for nested parsing, building and
response transfer, using an explicit live-allocation counter. It runs under
C++11/C++17 and array/vector caches. `command_catalog` checks registry coverage,
RESP round trips, binary/large unknown names and concurrent readers.
`command_metadata` compares wire replies and key extraction with the reference,
checks errors and binary keys, and exercises transactions and cluster routing.
`source_exec` additionally sweeps allocation failures around queued metadata
between writes and SELECT changes, checking that a single-store commit has no
partial prefix. The original ASan reproduction is retained locally at
`/tmp/pkv-command-memory-source.log`.

The final eight-configuration full regression passed 502/502 CTests. See the
[validation matrix](../tests/README.md#command-metadata-and-resp-ownership) for
backend/standard coverage, logs and reference limitations.
