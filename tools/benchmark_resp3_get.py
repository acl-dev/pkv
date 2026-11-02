#!/usr/bin/env python3
"""Compare PKV revisions with a checked, pipelined hot-key GET workload."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import platform
from statistics import median
import sys
import threading
from time import perf_counter

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
from integration_test import Client, server
from resp3_client import RespClient


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def run(binary, protocol, workers, batches, batch, backend='mdb'):
    payload = b'p' * 64
    wire = Client.encode(['GET', 'bench']) * batch
    expected = (b'$64\r\n' + payload + b'\r\n') * batch
    with server(binary, backend=backend, active_expire=False) as port:
        c = Client(port)
        assert c.call('SET', 'bench', payload) == b'OK'
        c.close()
        start = [None]
        ready = threading.Barrier(workers + 1,
            action=lambda: start.__setitem__(0, perf_counter()))

        def worker(_):
            c = RespClient(port)
            c.socket.settimeout(30)
            try:
                if protocol == 3:
                    assert c.call('HELLO', 3).kind == b'%'

                def exchange():
                    c.socket.sendall(wire)
                    data = bytearray()
                    while len(data) < len(expected):
                        part = c.socket.recv(len(expected) - len(data))
                        assert part
                        data += part
                    assert data == expected
                for _ in range(16):
                    exchange()
                ready.wait(timeout=30)
                for _ in range(batches):
                    exchange()
            finally:
                c.close()

        with ThreadPoolExecutor(max_workers=workers) as pool:
            futures = [pool.submit(worker, i) for i in range(workers)]
            ready.wait(timeout=30)
            for future in futures:
                future.result()
            elapsed = perf_counter() - start[0]
    return elapsed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', required=True)
    parser.add_argument('--pkv', required=True)
    parser.add_argument('--baseline-flags', required=True)
    parser.add_argument('--new-flags', required=True)
    parser.add_argument('--output', required=True)
    parser.add_argument('--workers', type=int, default=4)
    parser.add_argument('--batches', type=int, default=1024)
    parser.add_argument('--rounds', type=int, default=3)
    parser.add_argument('--backend', choices=['mdb', 'mdb_tbb'], default='mdb')
    parser.add_argument('--baseline-resp3', action='store_true')
    parser.add_argument('--comparison')
    parser.add_argument('--comparison-flags')
    args = parser.parse_args()
    assert 1 <= args.workers <= 32 and args.batches > 0 and args.rounds > 0
    flags, old_flags = Path(args.new_flags), Path(args.baseline_flags)
    assert flags.read_bytes() == old_flags.read_bytes(), 'compile flags changed'
    assert bool(args.comparison) == bool(args.comparison_flags)
    if args.comparison:
        assert flags.read_bytes() == Path(args.comparison_flags).read_bytes(), \
            'comparison compile flags changed'
    batch = 256
    variants = [('baseline_resp2', args.baseline, 2),
                ('new_resp2', args.pkv, 2),
                ('new_resp3', args.pkv, 3)]
    if args.baseline_resp3:
        variants.append(('baseline_resp3', args.baseline, 3))
    if args.comparison:
        variants.append(('comparison_resp2', args.comparison, 2))
    results = []
    for repetition in range(args.rounds):
        offset = repetition % len(variants)
        order = variants[offset:] + variants[:offset]
        for name, binary, protocol in order:
            elapsed = run(binary, protocol, args.workers, args.batches, batch,
                args.backend)
            requests = args.workers * args.batches * batch
            item = dict(variant=name, repetition=repetition + 1,
                        requests=requests, elapsed_seconds=elapsed,
                        requests_per_second=requests / elapsed)
            results.append(item)
            print(json.dumps(item), flush=True)
    summary = {name: median(r['requests_per_second'] for r in results
                           if r['variant'] == name) for name, _, _ in variants}
    baseline = summary['baseline_resp2']
    output = dict(results=results, median_requests_per_second=summary,
        delta_percent_vs_baseline={k: (v / baseline - 1) * 100
                                  for k, v in summary.items()},
        binaries={p: digest(p) for _, p, _ in variants},
        compile_flags_sha256=digest(flags), script_sha256=digest(__file__),
        platform=platform.platform(), backend=args.backend,
        build='C++11 Release',
        server_threads=2, workers=args.workers, batch=batch, payload_bytes=64,
        active_expire=False, warmup_batches_per_worker=16,
        limits=['One hot GET key; not mixed workloads or other backends',
                'Local machine and Python/socket client may limit throughput',
                'Interleaved samples; no statistical significance claim',
                'Compares PKV revisions, not stock Redis'])
    Path(args.output).write_text(json.dumps(output, indent=2) + '\n')
    print(json.dumps(summary), flush=True)


if __name__ == '__main__':
    main()
