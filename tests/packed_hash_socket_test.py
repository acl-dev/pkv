#!/usr/bin/env python3
"""Grouped Hash lifecycle: compare protocol results with isolated Redis."""
import argparse
import os
import random
import resource

from integration_test import Client, server


def run(port, reference):
    clients = [Client(port), Client(reference)]
    checks = 0

    def same(*args):
        nonlocal checks
        replies = [client.call(*args) for client in clients]
        assert type(replies[0]) is type(replies[1]) and replies[0] == replies[1], (args, replies)
        checks += 1
        return replies[0]

    def contents(key, expected):
        nonlocal checks
        for client in clients:
            reply = client.call('HGETALL', key)
            assert isinstance(reply, list) and len(reply) == 2 * len(expected)
            assert dict(zip(reply[::2], reply[1::2])) == expected
            checks += 1
        assert same('HLEN', key) == len(expected)

    expected = {('field:%013d' % i).encode(): b'val' for i in range(512)}
    flat = [part for pair in expected.items() for part in pair]
    deadline = 4102444800123
    key = 'grouped'
    try:
        assert same('HSET', key, *flat) == 512
        assert same('TYPE', key) == b'hash'
        assert same('PEXPIREAT', key, deadline) == 1
        contents(key, expected)
        fields = list(expected)
        same('HMGET', key, fields[0], 'absent', fields[-1])
        same('HSTRLEN', key, fields[0])
        same('HSETNX', key, fields[0], b'x' * 4096)
        same('HINCRBY', key, fields[0], 1)  # Invalid value must be atomic.
        same('HINCRBYFLOAT', key, fields[0], 'bad')
        contents(key, expected)
        same('HSET', key, fields[0], '1')
        same('HINCRBY', key, fields[0], 2)
        same('HINCRBYFLOAT', key, fields[0], '.5')
        expected[fields[0]] = b'3.5'
        assert same('PEXPIRETIME', key) == deadline
        for client in clients:
            for options in [(), ('NOVALUES',), ('MATCH', 'field:*7', 'COUNT', 3)]:
                cursor, found, rounds = 0, {}, 0
                while True:
                    cursor, values = client.call('HSCAN', key, cursor, *options)
                    if 'NOVALUES' in options:
                        found.update((field, expected[field]) for field in values)
                    else:
                        found.update(zip(values[::2], values[1::2]))
                    rounds += 1
                    assert rounds <= 1024
                    if int(cursor) == 0:
                        break
                wanted = {k: v for k, v in expected.items() if not options or
                          'MATCH' not in options or k.endswith(b'7')}
                assert found == wanted
            for count in [0, 1, 17, 600, -17]:
                values = client.call('HRANDFIELD', key, count, 'WITHVALUES')
                assert len(values) == 2 * (min(count, 512) if count >= 0 else -count)
                assert all(expected[f] == v for f, v in zip(values[::2], values[1::2]))
                if count > 0:
                    assert len(set(values[::2])) == len(values) // 2
        same('WATCH', key)
        same('HSETNX', key, fields[0], 'ignored')
        same('MULTI')
        same('HLEN', key)
        assert same('EXEC') == [512]
        same('WATCH', key)
        same('HSET', key, fields[0], '3.5')
        same('MULTI')
        same('HLEN', key)
        assert same('EXEC') is None
        assert same('COPY', key, 'copy') == 1
        same('RENAME', 'copy', 'renamed')
        contents('renamed', expected)
        assert same('PEXPIRETIME', 'renamed') == deadline
        assert same('COPY', key, 'cross-db', 'DB', 1) == 1
        same('SELECT', 1)
        contents('cross-db', expected)
        assert same('PEXPIRETIME', 'cross-db') == deadline
        assert same('MOVE', 'cross-db', 2) == 1
        same('SELECT', 2)
        contents('cross-db', expected)
        assert same('PEXPIRETIME', 'cross-db') == deadline
        same('SELECT', 0)
        same('PERSIST', 'renamed')
        assert same('PTTL', 'renamed') == -1
        commands = [('HSET', key, f, 'pipe') for f in fields[:64]]
        commands += [('HGET', key, f) for f in fields[:64]]
        for client in clients:
            client.socket.sendall(b''.join(Client.encode(args) for args in commands))
        replies = [[client.read() for _ in commands] for client in clients]
        assert replies[0] == replies[1] == [0] * 64 + [b'pipe'] * 64
        expected.update((f, b'pipe') for f in fields[:64])
        same('HGETDEL', key, 'FIELDS', 3, fields[0], fields[0], 'absent')
        del expected[fields[0]]
        contents(key, expected)
        # Large value converts to native pages; growth past 512 also falls back.
        same('HSET', key, fields[1], b'x' * 4096)
        expected[fields[1]] = b'x' * 4096
        contents(key, expected)
        same('HSET', 'renamed', 'extra', 'value')
        expanded = dict(expected)
        expanded.update((f, b'val') for f in fields)
        expanded[fields[0]] = b'3.5'
        expanded[b'extra'] = b'value'
        contents('renamed', expanded)
        # Delete to a compact root, keep expiry, then remove/recreate the key.
        same('HDEL', key, *fields[:480])
        expected = {f: v for f, v in expected.items() if f not in fields[:480]}
        contents(key, expected)
        assert same('PEXPIRETIME', key) == deadline
        same('HDEL', key, *expected)
        assert same('EXISTS', key) == 0
        same('HSET', key, 'new', 'value')
        assert same('PTTL', key) == -1
        contents(key, {b'new': b'value'})
        # Keep a grouped key under mixed staged insert/delete/numeric operations.
        rng = random.Random(20261108)
        same('HSET', 'churn', *[part for i in range(129)
                               for part in (('f%03d' % i), '0')])
        for step in range(500):
            field = 'f%03d' % rng.randrange(200)
            command = rng.choice(['HSET', 'HSETNX', 'HINCRBY', 'HDEL', 'HGETDEL', 'HMGET'])
            if command in ['HSET', 'HSETNX', 'HINCRBY']:
                same(command, 'churn', field, rng.randrange(10))
            elif command == 'HGETDEL':
                same(command, 'churn', 'FIELDS', 2, field, field)
            elif command == 'HMGET':
                same(command, 'churn', field, field, 'absent')
            else:
                same(command, 'churn', field, field)
            if step % 25 == 0:
                raw = clients[1].call('HGETALL', 'churn')
                contents('churn', dict(zip(raw[::2], raw[1::2])))
    finally:
        for client in clients:
            client.close()
    return checks


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--pkv', required=True)
    parser.add_argument('--redis', required=True)
    parser.add_argument('--backend', default=os.environ.get('PKV_TEST_BACKEND', 'mdb'))
    args = parser.parse_args()
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    for workers in [1, 4]:
        with server(args.redis, redis=True) as reference:
            with server(args.pkv, backend=args.backend, fiber_threads=workers) as port:
                checks = run(port, reference)
                print('%s workers=%d: %d protocol comparisons passed' %
                      (args.backend, workers, checks), flush=True)
