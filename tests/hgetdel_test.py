#!/usr/bin/env python3
"""HGETDEL replies, atomicity, WATCH, source EXEC and live replication."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import random
import time

from integration_test import Client, ErrorReply, server

FUTURE = 4102444800123


def exercise(port, reference):
    a, b = Client(port), Client(reference)
    checks = 0

    def compare(*args):
        nonlocal checks
        x, y = a.call(*args), b.call(*args)
        if args[0] == 'HGETALL':
            x = dict(zip(x[::2], x[1::2]))
            y = dict(zip(y[::2], y[1::2]))
        assert x == y, (args[:5], x, y)
        checks += 1
        return x

    try:
        for key in ['hash', b'', b'h\0x']:
            for value in [b'', b'0', b'001', b'a\0\xff', b'x' * 70000]:
                compare('HSET', key, b'', value, b'f\0x', value,
                        'keep', 'retained')
                compare('PEXPIREAT', key, FUTURE)
                compare('HGETDEL', key, 'fIeLdS', 5,
                        b'f\0x', b'f\0x', 'missing', b'', b'')
                compare('HGETALL', key)
                compare('PEXPIRETIME', key)
                compare('HGETDEL', key, 'FIELDS', 2, 'keep', 'keep')
                compare('TYPE', key)
                compare('HGETDEL', key, 'FIELDS', 1, 'missing')
        errors = [('missing', '1', 'a'), ('FIELDS', 'bad', 'a'),
                  ('FIELDS', '0', 'a'), ('FIELDS', '-1', 'a'),
                  ('FIELDS', '+1', 'a'), ('FIELDS', '01', 'a'),
                  ('FIELDS', '1.0', 'a'), ('FIELDS', b'1\0', 'a'),
                  ('FIELDS', '9223372036854775808', 'a'),
                  ('FIELDS', '9223372036854775807', 'a'),
                  ('FIELDS', '2', 'a'), ('FIELDS', '1', 'a', 'b')]
        for seed in [None, ('HSET', 'typed', 'a', 'v'),
                     ('SET', 'typed', 'str'), ('LPUSH', 'typed', 'v'),
                     ('SADD', 'typed', 'v'), ('ZADD', 'typed', 1, 'v')]:
            compare('DEL', 'typed')
            if seed:
                compare(*seed)
            for arguments in errors:
                compare('HGETDEL', 'typed', *arguments)
            compare('HGETDEL', 'typed', 'FIELDS', 2, 'a', 'a')
        for args in [(), ('hash',), ('hash', 'FIELDS'),
                     ('hash', 'FIELDS', '1')]:
            compare('HGETDEL', *args)
        compare('HSET', 'expired', 'a', 'old')
        compare('PEXPIREAT', 'expired', 1)
        compare('HGETDEL', 'expired', 'FIELDS', 2, 'a', 'a')
        rng = random.Random(837912)
        fields = [b'', b'a', b'b', b'f\0x', b'123']
        for _ in range(60):
            seed = rng.sample(fields, rng.randint(1, len(fields)))
            compare('HSET', 'model', *(v for field in seed for v in
                                     (field, bytes([rng.randrange(256)]))))
            selected = rng.choices(fields, k=rng.randint(1, 8))
            compare('HGETDEL', 'model', 'FIELDS', len(selected), *selected)
            compare('HGETALL', 'model')
        compare('SELECT', 1)
        compare('HSET', 'hash', 'a', 'db1', 'b', 'other')
        compare('MULTI')
        compare('HGETDEL', 'hash', 'FIELDS', 2, 'a', 'a')
        compare('HGETDEL', 'hash', 'FIELDS', 2, 'b')
        compare('HGET', 'hash', 'b')
        compare('HGETDEL', 'hash', 'FIELDS', 1, 'b')
        compare('TYPE', 'hash')
        compare('EXEC')
        compare('SELECT', 0)
        compare('HGET', 'hash', 'a')
        for command in [('HGETDEL', 'hash', 'FIELDS', 1, '{else}field'),
                        ('HGETDEL', 'hash', 'bad', 'bad', 'field')]:
            compare('COMMAND', 'GETKEYS', *command)
            compare('COMMAND', 'GETKEYSANDFLAGS', *command)
        compare('COMMAND', 'INFO', 'hgetdel')
        assert a.call('HSET', 'strict', 'a', 'old') == 1
        assert isinstance(a.call('HGETDEL', 'strict', b'FIELDS\0x', 1, 'a'),
                          ErrorReply)
        assert a.call('HGET', 'strict', 'a') == b'old'
        assert a.call('PING') == b'PONG'
        watch(port)
        watch(reference)
        race(port)
        race(reference)
        return checks
    finally:
        a.close()
        b.close()


def watch(port):
    a, b = Client(port), Client(port)
    try:
        assert a.call('HSET', 'watched', 'a', 'old', 'keep', 'yes') == 2
        for change in [False, True]:
            assert a.call('WATCH', 'watched') == b'OK'
            field = 'a' if change else 'missing'
            assert b.call('HGETDEL', 'watched', 'FIELDS', 1, field) == \
                ([b'old'] if change else [None])
            assert a.call('MULTI') == b'OK'
            assert a.call('HGET', 'watched', 'keep') == b'QUEUED'
            assert a.call('EXEC') == (None if change else [b'yes'])
        assert a.call('WATCH', 'watched') == b'OK'
        assert b.call('HGETDEL', 'watched', 'FIELDS', 1, 'keep') == [b'yes']
        assert a.call('MULTI') == b'OK'
        assert a.call('EXISTS', 'watched') == b'QUEUED'
        assert a.call('EXEC') is None
    finally:
        a.close()
        b.close()


def race(port):
    c = Client(port)
    try:
        assert c.call('HSET', 'race', 'a', 'one', 'b', 'two') == 2
    finally:
        c.close()

    def worker(_):
        c = Client(port)
        try:
            return c.call('HGETDEL', 'race', 'FIELDS', 2, 'a', 'b')
        finally:
            c.close()

    with ThreadPoolExecutor(max_workers=8) as pool:
        replies = list(pool.map(worker, range(8)))
    assert replies.count([b'one', b'two']) == 1, replies
    assert replies.count([None, None]) == 7, replies


def replication(binary):
    with server(binary) as source:
        rpc = '127.0.0.1:%d' % (source + 10000)
        with server(binary, replica_rpc_source=rpc) as receiver:
            a, b = Client(source), Client(receiver)
            try:
                deadline = time.monotonic() + 6
                while True:
                    assert a.call('SET', 'ready', 'yes') == b'OK'
                    if b.call('GET', 'ready') == b'yes':
                        break
                    assert time.monotonic() < deadline
                    time.sleep(0.02)
                big = b'x' * 70000
                assert a.call('HSET', 'copy', 'a', big,
                              'b', 'two', 'c', 'three') == 3
                assert a.call('PEXPIREAT', 'copy', FUTURE) == 1
                deadline = time.monotonic() + 6
                while b.call('PEXPIRETIME', 'copy') != FUTURE:
                    assert time.monotonic() < deadline
                    time.sleep(0.02)
                assert a.call('MULTI') == b'OK'
                assert a.call('HGETDEL', 'copy', 'FIELDS', 3,
                              'a', 'a', 'b') == b'QUEUED'
                assert a.call('HGET', 'copy', 'c') == b'QUEUED'
                assert a.call('EXEC') == [[big, None, b'two'], b'three']
                deadline = time.monotonic() + 6
                while b.call('HMGET', 'copy', 'a', 'b', 'c') != \
                        [None, None, b'three']:
                    assert time.monotonic() < deadline
                    time.sleep(0.02)
                assert b.call('PEXPIRETIME', 'copy') == FUTURE
                assert a.call('HGETDEL', 'copy', 'FIELDS', 1, 'c') == \
                    [b'three']
                deadline = time.monotonic() + 6
                while b.call('DBSIZE') != 1:
                    assert time.monotonic() < deadline
                    time.sleep(0.02)
            finally:
                a.close()
                b.close()


def cluster(binary):
    with server(binary, cluster=True) as port:
        c = Client(port)
        try:
            key = '{served}hash'
            assert isinstance(c.call('HGETDEL', key, 'FIELDS', 1, 'a'),
                              ErrorReply)
            slot = c.call('CLUSTER', 'KEYSLOT', key)
            assert c.call('CLUSTER', 'ADDSLOTS', slot) == b'OK'
            assert c.call('HSET', key, '{other}field', 'value') == 1
            assert c.call('HGETDEL', key, 'FIELDS', 1, '{other}field') == \
                [b'value']
        finally:
            c.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--pkv', required=True)
    parser.add_argument('--redis', required=True)
    args = parser.parse_args()
    with server(args.redis, redis=True) as ref, server(args.pkv) as port:
        checks = exercise(port, ref)
    replication(args.pkv)
    cluster(args.pkv)
    print('HGETDEL differential assertions:', checks)


if __name__ == '__main__':
    main()
