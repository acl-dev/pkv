#!/usr/bin/env python3
"""Bounded local SCAN comparison; temporary servers/data, no third-party modules.

Compares two pkv binaries, not Redis, and never asserts a performance advantage.
"""
import argparse
import json
import math
import os
import statistics
import time

from integration_test import Client, server


def pipeline(client, requests, expected):
    encoded = [Client.encode(request) for request in requests]
    chunks = [b''.join(encoded[i:i + 128]) for i in range(0, len(encoded), 128)]
    start = time.perf_counter()
    for i, chunk in enumerate(chunks):
        client.socket.sendall(chunk)
        for _ in range(min(128, len(requests) - i * 128)):
            reply = client.read()
            assert type(reply) is bytes and reply == expected, reply
    return len(requests) / (time.perf_counter() - start)


def measure(binary, key_count, hint, operations):
    value = b'v' * 128
    names = [b'bench:%08d\0key' % i for i in range(key_count)]
    expected_keys = set(names)
    with server(binary) as port:
        client = Client(port)
        try:
            pipeline(client, [('SET', name, value) for name in names], b'OK')
            durations, pages, cursor, seen = [], [], b'0', []
            start = time.perf_counter()
            while True:
                before = time.perf_counter()
                reply = client.call('SCAN', cursor, 'COUNT', hint)
                durations.append((time.perf_counter() - before) * 1e6)
                assert isinstance(reply, list) and len(reply) == 2 and reply[0].isdigit(), reply
                cursor, items = reply
                pages.append(len(items))
                seen.extend(items)
                if cursor == b'0':
                    break
            elapsed = (time.perf_counter() - start) * 1000
            assert len(seen) == key_count and set(seen) == expected_keys
            ordered = sorted(durations)
            set_rate = pipeline(client, [('SET', names[i % key_count], value) for i in range(operations)], b'OK')
            get_rate = pipeline(client, [('GET', names[i % key_count]) for i in range(operations)], value)
            return dict(scan_pages=len(pages), scan_max_keys=max(pages),
                        scan_p50_us=statistics.median(ordered),
                        scan_p95_us=ordered[max(0, math.ceil(len(ordered) * .95) - 1)],
                        scan_total_ms=elapsed, pipeline_set_ops_s=set_rate, pipeline_get_ops_s=get_rate)
        finally:
            client.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--before', required=True)
    parser.add_argument('--after', required=True)
    parser.add_argument('--keys', type=int, default=30000)
    parser.add_argument('--count', type=int, default=10)
    parser.add_argument('--rounds', type=int, default=5)
    parser.add_argument('--operations', type=int, default=10000)
    args = parser.parse_args()
    if min(args.keys, args.count, args.rounds, args.operations) < 1:
        parser.error('all numeric parameters must be positive')
    results = {'before': [], 'after': []}
    for round_number in range(args.rounds):
        order = ('before', 'after') if round_number % 2 == 0 else ('after', 'before')
        for name in order:
            result = measure(os.path.abspath(getattr(args, name)), args.keys, args.count, args.operations)
            results[name].append(result)
            print(json.dumps(dict(round=round_number + 1, binary=name, **result)), flush=True)
    medians = {name: {key: statistics.median(row[key] for row in rows) for key in rows[0]}
               for name, rows in results.items()}
    print(json.dumps(dict(keys=args.keys, count=args.count, rounds=args.rounds, workers=2,
                          shards=16, value_bytes=128, pipeline_depth=128,
                          summary='median of per-round metrics', results=medians)), flush=True)


if __name__ == '__main__':
    main()
