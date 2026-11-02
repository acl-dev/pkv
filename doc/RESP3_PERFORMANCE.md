# RESP3 change: local hot-key GET comparison

This comparison uses a retained pre-RESP3 PKV executable and the updated server,
both C++11 Release with identical compiler flags and the native memory backend.
It is not a stock Redis comparison. The workload uses the same two server
threads, four clients, 64-byte payload, one hot key and 256 pipelined GETs per
exchange. Active expiry is disabled in both variants. It checks every response
byte and warms up each connection before synchronizing the timed workers.

Three rounds rotate baseline RESP2, new RESP2 and new RESP3 order. Each sample
executes 4,194,304 measured requests, excluding its warmup batches. The report
retains every sample, medians, elapsed times, binary/compiler-flag/script hashes
and platform information. Three interleaved samples do not establish statistical
significance. Python/client limits, OS scheduling and local CPU state can affect
results; mixed operations, tail latency, other backends and longer workloads
need independent measurements.

Reproduce with matching builds and a copy of the old executable/flags retained
before rebuilding it. For the current local paths:

```sh
python3 tools/benchmark_resp3_get.py \
  --baseline /tmp/pkv-resp3-baseline-c11-pkv \
  --pkv /tmp/pkv-c11-build/pkv \
  --baseline-flags /tmp/pkv-resp3-baseline-c11-flags.make \
  --new-flags /tmp/pkv-c11-build/CMakeFiles/pkv_core.dir/flags.make \
  --batches 4096 \
  --output doc/benchmarks/resp3-get-performance-2026-10-31.json
```

The script requires exact compiler-flag file equality. A local loopback listener
must be permitted. Raw process logs remain under the normal temporary integration
server directories during execution; the benchmark emits each checked sample
to stdout and saves the final JSON report. This is an encoding/command-path
regression check for one workload, not a general throughput claim.


The retained extended run checked 37,748,736 measured replies. Median GET rates
were approximately 1,544,341/s for baseline RESP2, 1,530,273/s for new RESP2
(-0.91%) and 1,554,610/s for new RESP3 (+0.66%). Both observed differences are
within about one percent of this baseline; no performance improvement or
statistical significance is inferred. The shorter initial samples are retained
separately, rather than silently overwritten.

See [extended samples and binary/flag hashes](
benchmarks/resp3-get-performance-2026-10-31.json) and
[initial shorter samples](
benchmarks/resp3-get-performance-initial-2026-10-31.json). Raw stdout logs are
`/tmp/pkv-resp3-get-bench-long.log` and `/tmp/pkv-resp3-get-bench.log`.
