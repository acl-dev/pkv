#!/usr/bin/env python3
"""SET IFEQ/IFNE differential, CAS, WATCH and source-workspace checks."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import time

from integration_test import Client, ErrorReply, server


def exercise(port, reference):
    a, b = Client(port), Client(reference)
    checks = 0

    def compare(*args):
        nonlocal checks
        actual, expected = a.call(*args), b.call(*args)
        assert actual == expected, (args[:3], args[3:], actual, expected)
        checks += 1
        return actual

    try:
        for old in [b'', b'012', b'\0binary\xff', b'a' * 17000]:
            for condition in ['IFEQ', 'IFNE']:
                for get in [False, True]:
                    for match in [old, old + b'\0']:
                        compare('SET', 'key', old, 'PXAT', 4102444800123)
                        options = ['GET'] if get else []
                        compare('SET', 'key', 'new', condition, match,
                                'KEEPTTL', *options)
                        compare('GET', 'key')
                        compare('PEXPIRETIME', 'key')
        for condition, rejected, accepted in [
                ('IFEQ', 'wrong', 'GET'), ('IFNE', 'GET', 'wrong')]:
            compare('SET', 'key', 'GET', 'PXAT', 4102444800123)
            compare('SET', 'key', 'new', condition, rejected, 'PXAT', 1)
            compare('GET', 'key')
            compare('PEXPIRETIME', 'key')
            compare('SET', 'key', 'new', condition, accepted, 'PXAT', 1,
                    'GET')
            compare('GET', 'key')
        for condition in ['IFEQ', 'IFNE']:
            for get in [False, True]:
                compare('DEL', 'missing')
                compare('SET', 'missing', 'new', condition, '',
                        *(['GET'] if get else []))
                compare('GET', 'missing')
                compare('SET', 'expired', 'old', 'PXAT', 1)
                compare('SET', 'expired', 'new', condition, 'old')
                compare('GET', 'expired')
                for seed in [('LPUSH', 'typed', 'x'),
                             ('HSET', 'typed', 'x', 'v'),
                             ('SADD', 'typed', 'x'),
                             ('ZADD', 'typed', '1', 'x')]:
                    compare('DEL', 'typed')
                    compare(*seed)
                    compare('SET', 'typed', 'new', condition, 'x',
                            *(['GET'] if get else []))
                    compare('TYPE', 'typed')
        for flags in [('IFEQ', 'a', 'IFEQ', 'b'),
                      ('IFNE', 'a', 'IFNE', 'b'),
                      ('NX', 'IFEQ', 'a'), ('XX', 'IFNE', 'a'),
                      ('IFEQ', 'a', 'IFNE', 'b'), ('IFEQ',),
                      ('IFNE',), ('IFEQ', 'a', 'unknown'),
                      ('IFEQ', 'a', 'EX', 0),
                      ('IFNE', 'a', 'PX', 'bad')]:
            compare('SET', 'key', 'b')
            compare('SET', 'key', 'new', *flags)
            compare('GET', 'key')
        compare('SET', b'k\0x', b'a\0b')
        compare('SET', b'k\0x', 'new', 'iFeQ', b'a\0b', 'GET')
        compare('SET', 'key', 'a')
        compare('MULTI')
        compare('SET', 'key', 'b', 'IFEQ', 'a')
        compare('SET', 'key', 'c', 'IFNE', 'a', 'GET')
        compare('GET', 'key')
        compare('EXEC')
        compare('GET', 'key')
        compare('SELECT', 1)
        compare('SET', 'key', 'db1', 'IFNE', 'c')
        compare('GET', 'key')
        compare('SELECT', 0)
        compare('GET', 'key')
        # PKV rejects NUL-suffixed option tokens rather than C-string prefixes.
        for token in [b'IFEQ\0x', b'IFNE\0x', b'NX\0x', b'GET\0x']:
            operand = ['c'] if token.startswith(b'IF') else []
            assert isinstance(a.call('SET', 'key', 'new', token, *operand),
                              ErrorReply)
            assert a.call('GET', 'key') == b'c'
        for flags in [('IFEQ', 'c', 'NX'), ('IFNE', 'c', 'XX')]:
            assert a.call('SET', 'key', 'new', *flags) == ErrorReply(
                b'ERR syntax error')
            assert a.call('GET', 'key') == b'c'
        watch_noop(port)
        watch_noop(reference)
        return checks
    finally:
        a.close()
        b.close()


def watch_noop(port):
    a, b = Client(port), Client(port)
    try:
        assert a.call('SET', 'watched', 'old') == b'OK'
        assert a.call('WATCH', 'watched') == b'OK'
        assert b.call('SET', 'watched', 'new', 'IFEQ', 'wrong') is None
        assert a.call('MULTI') == b'OK'
        assert a.call('GET', 'watched') == b'QUEUED'
        assert a.call('EXEC') == [b'old']
        assert a.call('WATCH', 'watched') == b'OK'
        assert b.call('SET', 'watched', 'new', 'IFEQ', 'old') == b'OK'
        assert a.call('MULTI') == b'OK'
        assert a.call('GET', 'watched') == b'QUEUED'
        assert a.call('EXEC') is None
    finally:
        a.close()
        b.close()


def cas_race(port):
    c = Client(port)
    try:
        assert c.call('SET', 'race', 'old') == b'OK'
    finally:
        c.close()

    def worker(index):
        local = Client(port)
        try:
            return local.call('SET', 'race', index, 'IFEQ', 'old')
        finally:
            local.close()

    with ThreadPoolExecutor(max_workers=8) as pool:
        replies = list(pool.map(worker, range(8)))
    assert replies.count(b'OK') == 1 and replies.count(None) == 7, replies


def replication(binary):
    with server(binary) as primary:
        source = '127.0.0.1:%d' % (primary + 10000)
        with server(binary, replica_rpc_source=source) as replica:
            a, b = Client(primary), Client(replica)
            try:
                # PING readiness does not imply RPC stream registration.
                # This checks live delivery after readiness, not initial sync.
                deadline = time.monotonic() + 6
                while True:
                    assert a.call('SET', 'ready', 'yes') == b'OK'
                    if b.call('GET', 'ready') == b'yes':
                        break
                    assert time.monotonic() < deadline
                    time.sleep(0.02)
                assert a.call('SET', 'copy', 'old') == b'OK'
                deadline = time.monotonic() + 6
                while b.call('GET', 'copy') != b'old':
                    assert time.monotonic() < deadline
                    time.sleep(0.02)
                assert a.call('MULTI') == b'OK'
                assert a.call('SET', 'copy', b'new\0v', 'IFEQ', 'old') == \
                    b'QUEUED'
                assert a.call('SET', 'copy', 'bad', 'IFNE', b'new\0v') == \
                    b'QUEUED'
                assert a.call('EXEC') == [b'OK', None]
                deadline = time.monotonic() + 6
                while b.call('GET', 'copy') != b'new\0v':
                    assert time.monotonic() < deadline
                    time.sleep(0.02)
            finally:
                a.close()
                b.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--pkv', required=True)
    parser.add_argument('--redis', required=True)
    args = parser.parse_args()
    with server(args.redis, redis=True) as ref, server(args.pkv) as port:
        checks = exercise(port, ref)
        cas_race(port)
        cas_race(ref)
    replication(args.pkv)
    print('SET IFEQ/IFNE differential assertions:', checks)


if __name__ == '__main__':
    main()
