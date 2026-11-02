# Authentication dispatch: local GET comparison

A retained pre-authentication C++11 Release PKV binary is compared with the new
server using the same compiler flags, two memory-server threads, four clients,
64-byte payload and 256-command pipelines. Active expiry is disabled in every
variant. The policy is open in this benchmark; the timed command path checks the
same true connection-authenticated flag used after login. Login, password checks
and RPC handshakes are excluded, so this is not an authentication-cost benchmark.

Three rounds rotate the baseline RESP2/new RESP2/new RESP3 order. Every sample
checks 4,194,304 complete GET replies, following synchronized per-client warmup.
The 37,748,736 measured replies all matched their expected bytes. Median rates
were 1,495,670/s for baseline RESP2, 1,512,780/s for new RESP2 (+1.14%) and
1,474,567/s for new RESP3 (-1.41%). Differences are within about 1.5% in this run;
no general speedup or statistical significance is inferred. This compares PKV
revisions, not stock Redis, and does not cover other backends, disjoint keys,
mixed operations, tail latency or long-running load.

The measured server executable is retained at `/tmp/pkv-auth-measured-c11-pkv`.
The final entry-file formatting change only wraps an expression and shifts
later diagnostic line numbers; its binaries were rebuilt and authentication
checks repeated separately. Reproduce the measured comparison with:

```sh
python3 tools/benchmark_resp3_get.py \
  --baseline /tmp/pkv-auth-baseline-c11-pkv \
  --pkv /tmp/pkv-auth-measured-c11-pkv \
  --baseline-flags /tmp/pkv-auth-baseline-c11-flags.make \
  --new-flags /tmp/pkv-c11-build/CMakeFiles/pkv_core.dir/flags.make \
  --batches 4096 \
  --output doc/benchmarks/auth-get-performance-2026-10-31.json
```

The script requires equal compiler-flag files. All per-sample rates, elapsed
times, binary/compiler/script hashes and platform information remain in the
[JSON report](benchmarks/auth-get-performance-2026-10-31.json). Raw stdout is
retained at `/tmp/pkv-auth-get-bench.log`. Desktop scheduling, CPU state and the
Python/socket client can affect results; stronger performance conclusions need
broader controlled measurements.
