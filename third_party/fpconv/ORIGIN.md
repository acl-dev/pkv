# fpconv source

These unmodified files were copied from `deps/fpconv` in the local Redis 8.6.2
source distribution on 2026-10-30. They originate from night-shift/fpconv and
include the Redis-maintained power table. Each source retains its copyright
notice and Boost Software License 1.0; see LICENSE.txt.

Only this standalone conversion dependency is vendored. No Redis command
implementation is copied. pkv uses fpconv for C++11-compatible, locale-independent
double formatting; parsing and command/storage behavior are implemented by pkv.
