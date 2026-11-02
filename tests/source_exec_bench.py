#!/usr/bin/env python3
"""Local before/after source EXEC benchmark; no upstream Redis comparison."""
import argparse
import hashlib
import json
from pathlib import Path
import statistics
import time

from integration_test import Client, server


def sample(binary, backend, groups, width, mode, keyspace):
    with server(binary, backend=backend) as port:
        client = Client(port)
        try:
            commands = [('SET', 'bench-%d' % (i % keyspace), b'x' * 64)
                        for i in range(width)]
            if mode == 'exec':
                commands = [('MULTI',)] + commands + [('EXEC',)]
            wire = b''.join(Client.encode(args) for args in commands)

            def run():
                start = time.perf_counter()
                client.socket.sendall(wire)
                if mode == 'exec':
                    assert client.read() == b'OK'
                    for _ in range(width):
                        assert client.read() == b'QUEUED'
                    assert client.read() == [b'OK'] * width
                else:
                    for _ in range(width):
                        assert client.read() == b'OK'
                return time.perf_counter() - start

            for _ in range(20):
                run()
            timings = []
            start = time.perf_counter()
            for _ in range(groups):
                timings.append(run())
            elapsed = time.perf_counter() - start
            keys = ['bench-%d' % (i % keyspace) for i in range(width)]
            assert client.call('MGET', *keys) == [b'x' * 64] * width
            return {'seconds': elapsed, 'writes_per_second': groups * width /
                    elapsed, 'median_group_ms': statistics.median(timings) *
                    1000, 'p95_group_ms': sorted(timings)[
                        int(len(timings) * .95)] * 1000}
        finally:
            client.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--before', required=True)
    parser.add_argument('--after', required=True)
    parser.add_argument('--backend', required=True)
    parser.add_argument('--output', required=True)
    parser.add_argument('--groups', type=int, default=100)
    parser.add_argument('--width', type=int, default=32)
    parser.add_argument('--samples', type=int, default=3)
    parser.add_argument('--keyspace', type=int)
    parser.add_argument('--mode', choices=['exec', 'ordinary'], default='exec')
    args = parser.parse_args()
    if min(args.groups, args.width, args.samples) < 1:
        parser.error('groups, width and samples must be positive')
    if args.keyspace is None:
        args.keyspace = args.width
    if not 1 <= args.keyspace <= args.width:
        parser.error('keyspace must be between 1 and width')
    result = {'backend': args.backend, 'mode': args.mode, 'groups': args.groups,
              'width': args.width, 'samples': args.samples,
              'keyspace': args.keyspace,
              'warmup_groups': 20, 'rows': []}
    # Alternate order to reduce systematic warm-cache/thermal bias.
    for index in range(args.samples):
        variants = [('before', args.before), ('after', args.after)]
        if index % 2:
            variants.reverse()
        for label, binary in variants:
            row = sample(binary, args.backend, args.groups, args.width,
                         args.mode, args.keyspace)
            row.update(variant=label, sample=index + 1)
            result['rows'].append(row)
    result['binaries'] = {label: {'path': str(Path(binary).resolve()),
        'sha256': hashlib.sha256(Path(binary).read_bytes()).hexdigest()}
        for label, binary in [('before', args.before), ('after', args.after)]}
    Path(args.output).write_text(json.dumps(result, indent=2) + '\n')
    for label in ['before', 'after']:
        values = [row['writes_per_second'] for row in result['rows']
                  if row['variant'] == label]
        print(args.backend, label, 'median writes/s',
              round(statistics.median(values), 1))


if __name__ == '__main__':
    main()
