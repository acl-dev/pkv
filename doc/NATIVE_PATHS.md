# Exact native storage roots

Native startup must preserve each configured root after canonicalization.
The `dbpath` configuration keeps its existing delimiter-list syntax. A token
can be an alias whose target contains spaces, commas, semicolons or tabs.
Those characters in the resolved directory name are filesystem data.

The source688 control reproduced a regression in both RocksDB and WiredTiger:
an existing unbound store opened through such an alias with the journal set to
`off`, but default-auto startup rejected the canonical name. The original data
remained readable after the rejected startup. The earlier restriction avoided
reparsing an ambiguously joined list when passing roots into native engines.

## Startup interface

The master now passes native roots as an ordered vector. Logical DB suffixes
are appended to individual paths. `db::open_paths` and `open_observed_paths`
dispatch to native engines without encoding the vector into a string.
The observed form retains the shared observer owner, just like `open_observed`.
Unsupported engines decline this explicit API; their old startup entry remains.

RocksDB and WiredTiger validate every element for empty names and embedded NUL
before opening any store. Their existing string API still parses the old list
syntax and then uses the same native opening implementation. This changes no
per-key state, collection page format or ordinary command path.

Domain markers still bind backend, DB count, ordered canonical roots and journal
path using the existing bounded binary identity. Alternate aliases resolving to
the same roots preserve that identity. Duplicate roots, changed ordering,
damaged markers and disabling a bound journal retain their existing rejection
rules. No migration or unbinding protocol is added.

This does not add quoting or escaping to `dbpath`. Literal delimiters in a
configuration token retain their old meaning; use an existing unambiguous alias
when needed. Explicit journal paths are a single path, not a root list.

## Validation in progress

`move_domain` covers canonical names containing spaces, semicolons, commas,
tabs, LF and CR, with binding and exact identity on reopening.

`native_paths_rdb/wdb` invokes a direct C++ helper that checks invalid elements
before file creation, exact multi-root writes/reopening, observer ownership
and explicit refusal on a memory backend. Its frontend fixture then upgrades
an unbound store, migrates five paged types, preserves absolute expiry, reopens
through alternate aliases, checks neighboring DBs and verifies an explicit
journal whose control directory contains spaces and delimiters.

The C++11 focus selection and RocksDB path fixture have passed. WiredTiger
and the complete configuration matrix still require validation before this
change is accepted. The full Redis goal remains active; see the
[stage roadmap](ROADMAP_STATUS.md).
