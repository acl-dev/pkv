#!/usr/bin/env python3
"""Checked PKV revision comparisons for overwrite, churn and retained fills."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import platform
from statistics import median
import subprocess
import sys
import threading
from time import perf_counter

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
from integration_test import Client, server


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def resident(pid):
    return int(subprocess.check_output(
        ['ps', '-o', 'rss=', '-p', str(pid)], text=True).strip()) * 1024


def run(binary, backend, scenario, batches, workers=4, batch=256):
    payload = b'p' * 64
    process = {}
    with server(binary, backend=backend, active_expire=False,
                process_info=process) as port:
        before = resident(process['pid'])
        start = [None]
        ready = threading.Barrier(workers + 1,
            action=lambda: start.__setitem__(0, perf_counter()))
        def worker(index):
            client = Client(port)
            client.socket.settimeout(30)
            hot = 'hot:' + str(index)
            try:
                # Warm request/parser state, then start with no retained roots.
                for _ in range(16):
                    assert client.call('SET', hot, payload) == b'OK'
                    assert client.call('DEL', hot) == 1
                if scenario == 'overwrite':
                    assert client.call('SET', hot, payload) == b'OK'
                def exchange(wire, expected):
                    client.socket.sendall(wire)
                    data = bytearray()
                    while len(data) < len(expected):
                        part = client.socket.recv(len(expected) - len(data))
                        assert part
                        data += part
                    assert data == expected
                # Build client input before timing; server values stay small.
                # in the server: each root's logical value is exactly 64 bytes.
                wires = []
                for iteration in range(batches):
                    if scenario == 'overwrite':
                        wire = Client.encode(['SET', hot, payload]) * batch
                        expected = b'+OK\r\n' * batch
                    else:
                        parts = []
                        for offset in range(batch):
                            key = '%d:%d:%d' % (index, iteration, offset)
                            parts.append(Client.encode(['SET', key, payload]))
                            if scenario == 'churn':
                                parts.append(Client.encode(['DEL', key]))
                        wire = b''.join(parts)
                        expected = (b'+OK\r\n:1\r\n' if scenario == 'churn'
                                    else b'+OK\r\n') * batch
                    wires.append((wire, expected))
                ready.wait(timeout=30)
                for wire, expected in wires:
                    exchange(wire, expected)
            except BaseException:
                ready.abort(); raise
            finally:
                client.close()
        with ThreadPoolExecutor(max_workers=workers) as pool:
            futures = [pool.submit(worker, i) for i in range(workers)]
            ready.wait(timeout=30)
            for future in futures:
                future.result()
            elapsed = perf_counter() - start[0]
        after = resident(process['pid'])
        client = Client(port)
        try:
            roots = client.call('DBSIZE')
            expected = workers if scenario == 'overwrite' else (
                workers * batches * batch if scenario == 'fill' else 0)
            assert roots == expected, (roots, expected)
            if scenario == 'fill':
                assert client.call('GET', '0:0:0') == payload
                assert client.call('GET', '%d:%d:%d' %
                    (workers - 1, batches - 1, batch - 1)) == payload
        finally:
            client.close()
    operations = workers * batches * batch * (2 if scenario == 'churn' else 1)
    return {'operations': operations, 'seconds': elapsed,
        'operations_per_second': operations / elapsed,
        'resident_before_bytes': before, 'resident_after_bytes': after,
        'retained_roots': roots}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ['baseline', 'pkv', 'baseline-flags', 'new-flags', 'output']:
        parser.add_argument('--' + name, required=True)
    parser.add_argument('--backend', choices=['mdb', 'mdb_tbb'], required=True)
    parser.add_argument('--batches', type=int, default=256)
    parser.add_argument('--rounds', type=int, default=3)
    args = parser.parse_args()
    assert Path(args.baseline_flags).read_bytes() == \
        Path(args.new_flags).read_bytes(), 'compile flags changed'
    assert args.batches > 0 and args.rounds > 0
    variants = [('baseline', args.baseline), ('new', args.pkv)]
    results = []
    for scenario in ['overwrite', 'churn', 'fill']:
        for repetition in range(args.rounds):
            order = variants[repetition % 2:] + variants[:repetition % 2]
            for variant, binary in order:
                row = run(binary, args.backend, scenario, args.batches)
                row.update(variant=variant, repetition=repetition + 1,
                    scenario=scenario)
                results.append(row)
                print(json.dumps(row), flush=True)
    summaries = []
    for scenario in ['overwrite', 'churn', 'fill']:
        selected = {name: [r for r in results if r['scenario'] == scenario
            and r['variant'] == name] for name, _ in variants}
        rates = {name: median(r['operations_per_second'] for r in rows)
            for name, rows in selected.items()}
        rss = {name: median(r['resident_after_bytes'] for r in rows)
            for name, rows in selected.items()}
        summaries.append({'scenario': scenario, 'median_ops_per_second': rates,
            'rate_delta_percent': 100 * (rates['new'] / rates['baseline'] - 1),
            'median_resident_bytes': rss})
    output = {'backend': args.backend, 'results': results,
        'summaries': summaries, 'platform': platform.platform(),
        'binaries': {p: digest(p) for _, p in variants},
        'flags_sha256': digest(args.new_flags),
        'script_sha256': digest(__file__), 'workers': 4, 'batch': 256,
        'value_bytes': 64, 'protocol': 2,
        'limits': ['Three rotating PKV revision samples, not official Redis',
            'RSS includes all runtime/allocator/cache memory, not index bytes',
            'Cold fill retains 262144 roots with the default batch count',
            'Client builds requests outside timing; this is a local workload',
            'No statistical significance or general mixed-workload claim']}
    Path(args.output).write_text(json.dumps(output, indent=2) + '\n')


if __name__ == '__main__':
    main()
