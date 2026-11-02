#!/usr/bin/env python3
"""MSETEX TTL, atomicity, routing, WATCH, EXEC and replication checks."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import time

from integration_test import Client, ErrorReply, server

FUTURE = 4102444800123


def exercise(port, reference):
    a, b = Client(port), Client(reference)
    checks = 0

    def compare(*args):
        nonlocal checks
        actual, expected = a.call(*args), b.call(*args)
        assert actual == expected, (args[:4], args[-3:], actual, expected)
        checks += 1
        return actual

    try:
        for live in [0, 1, 2]:
            for condition in [[], ['NX'], ['XX']]:
                for expiry in [[], ['KEEPTTL'], ['PXAT', FUTURE + 1000],
                               ['EXAT', 4102444800], ['PXAT', 1]]:
                    compare('DEL', 'a', 'b')
                    if live:
                        compare('SET', 'a', 'old-a', 'PXAT', FUTURE)
                    if live == 2:
                        compare('HSET', 'b', 'field', 'old-b')
                        compare('PEXPIREAT', 'b', FUTURE + 100)
                    compare('MSETEX', 2, 'a', b'new\0a', 'b', b'b' * 17000,
                            *condition, *expiry)
                    compare('MGET', 'a', 'b')
                    compare('TYPE', 'a')
                    compare('TYPE', 'b')
                    compare('PEXPIRETIME', 'a')
                    compare('PEXPIRETIME', 'b')
        for condition in [[], ['NX'], ['XX']]:
            compare('DEL', 'dup', 'other')
            compare('MSETEX', 3, 'dup', 'first', 'other', 'middle',
                    'dup', b'last\0', *condition, 'KEEPTTL')
            compare('MGET', 'dup', 'other')
        for value in [b'', b'\0\xff', b'x' * 70000]:
            compare('MSETEX', 2, b'', value, b'k\0x', value,
                    'PXAT', FUTURE)
            compare('MGET', b'', b'k\0x')
            compare('PEXPIRETIME', b'')
            compare('PEXPIRETIME', b'k\0x')
        for count in ['bad', '0', '-1', '01', '+1', '2147483648',
                      '9223372036854775808', b'1\0']:
            compare('MSETEX', count, 'a', 'v')
        for args in [('MSETEX',), ('MSETEX', 1), ('MSETEX', 1, 'a'),
                     ('MSETEX', 2, 'a', 'v'),
                     ('MSETEX', 2, 'a', 'v', 'NX')]:
            compare(*args)
        for flags in [('NX', 'XX'), ('XX', 'NX'), ('NX', 'NX'),
                      ('XX', 'XX'), ('KEEPTTL', 'KEEPTTL'),
                      ('EX', 1, 'PX', 1), ('KEEPTTL', 'EX', 1),
                      ('GET',), ('IFEQ', 'x'), ('IFDEQ', '0' * 16),
                      ('EX',), ('EX', 0), ('PX', -1), ('EX', 'bad'),
                      ('EX', 9223372036854775807),
                      ('PXAT', 9223372036854775808),
                      ('EX', 'bad', 'EX', 30)]:
            compare('SET', 'a', 'original', 'PXAT', FUTURE)
            compare('MSETEX', 1, 'a', 'replacement', *flags)
            compare('GET', 'a')
            if 'EX' not in flags or flags[-2:] != ('EX', 30):
                compare('PEXPIRETIME', 'a')
        compare('MSETEX', 2, 'a', 'db0', 'b', 'db0')
        compare('SELECT', 1)
        compare('MSETEX', 2, 'a', 'db1', 'b', 'db1', 'NX')
        compare('MGET', 'a', 'b')
        compare('SELECT', 0)
        compare('MGET', 'a', 'b')
        compare('MULTI')
        compare('MSETEX', 2, 'a', 'one', 'b', 'two', 'XX', 'PXAT', FUTURE)
        compare('MSETEX', 2, 'a', 'bad', 'missing', 'bad', 'XX')
        compare('MGET', 'a', 'b')
        compare('PEXPIRETIME', 'a')
        compare('EXEC')
        compare('MGET', 'a', 'b')
        for args in [('MSETEX', 2, 'a', '{other}value', 'b', 'PX', 'NX'),
                     ('MSETEX', 2, 'dup', 'v', 'dup', 'x', 'KEEPTTL'),
                     ('MSETEX', 0, 'a', 'b'),
                     ('MSETEX', 2, 'a', 'b'),
                     ('MSETEX', 2147483648, 'a', 'b')]:
            compare('COMMAND', 'GETKEYS', *args)
            compare('COMMAND', 'GETKEYSANDFLAGS', *args)
        for option in [b'NX\0x', b'XX\0x', b'EX\0x', b'KEEPTTL\0x']:
            extra = [30] if option.startswith(b'EX') else []
            assert a.call('SET', 'strict', 'old') == b'OK'
            assert a.call('MSETEX', 1, 'strict', 'new', option,
                          *extra) == ErrorReply(b'ERR syntax error')
            assert a.call('GET', 'strict') == b'old'
        assert a.call('SET', 'overflow', 'old', 'PXAT', FUTURE) == b'OK'
        assert a.call('MSETEX', 1, 'overflow', 'new', 'PX',
                      9223372036854775807) == ErrorReply(
                          b"ERR invalid expire time in 'msetex' command")
        assert a.call('GET', 'overflow') == b'old'
        assert a.call('PEXPIRETIME', 'overflow') == FUTURE
        relative(port)
        relative(reference)
        watches(port)
        watches(reference)
        return checks
    finally:
        a.close()
        b.close()


def relative(port):
    c = Client(port)
    try:
        for option, amount, scale in [('EX', 30, 1000), ('PX', 30000, 1)]:
            start = int(time.time() * 1000)
            assert c.call('MSETEX', 2, 'relative-a', 'a', 'relative-b', 'b',
                          option, amount) == 1
            end = int(time.time() * 1000)
            a = c.call('PEXPIRETIME', 'relative-a')
            b = c.call('PEXPIRETIME', 'relative-b')
            assert a == b and start + amount * scale <= a <= \
                end + amount * scale
    finally:
        c.close()


def watches(port):
    a, b = Client(port), Client(port)
    try:
        assert a.call('SET', 'watched', 'old') == b'OK'
        assert a.call('WATCH', 'watched') == b'OK'
        assert b.call('MSETEX', 2, 'watched', 'new', 'missing-xx', 'new',
                      'XX') == 0
        assert a.call('MULTI') == b'OK'
        assert a.call('GET', 'watched') == b'QUEUED'
        assert a.call('EXEC') == [b'old']
        assert a.call('WATCH', 'watched') == b'OK'
        assert b.call('MSETEX', 2, 'watched', 'new', 'other-watch', 'new') == 1
        assert a.call('MULTI') == b'OK'
        assert a.call('GET', 'watched') == b'QUEUED'
        assert a.call('EXEC') is None
        assert a.call('DEL', 'past-watch') in (0, 1)
        assert a.call('WATCH', 'past-watch') == b'OK'
        assert b.call('MSETEX', 1, 'past-watch', 'v', 'PXAT', 1) == 1
        assert a.call('MULTI') == b'OK'
        assert a.call('EXISTS', 'past-watch') == b'QUEUED'
        assert a.call('EXEC') is None
    finally:
        a.close()
        b.close()


def race(port):
    c = Client(port)
    try:
        c.call('DEL', 'race-a', 'race-b')
    finally:
        c.close()

    def worker(index):
        client = Client(port)
        try:
            return client.call('MSETEX', 2, 'race-a', index, 'race-b', index,
                               'NX', 'KEEPTTL')
        finally:
            client.close()

    with ThreadPoolExecutor(max_workers=8) as pool:
        replies = list(pool.map(worker, range(8)))
    assert replies.count(1) == 1 and replies.count(0) == 7, replies


def replication(binary):
    with server(binary) as primary:
        source = '127.0.0.1:%d' % (primary + 10000)
        with server(binary, replica_rpc_source=source) as replica:
            a, b = Client(primary), Client(replica)
            try:
                deadline = time.monotonic() + 6
                while True:
                    assert a.call('SET', 'ready', 'yes') == b'OK'
                    if b.call('GET', 'ready') == b'yes':
                        break
                    assert time.monotonic() < deadline
                    time.sleep(0.02)
                values = [b'a' * 70000, b'b' * 40000]
                assert a.call('MSETEX', 2, 'a', values[0], 'b', values[1],
                              'PXAT', FUTURE) == 1
                deadline = time.monotonic() + 6
                while True:
                    current = b.call('MGET', 'a', 'b')
                    assert current in ([None, None], values)
                    if current == values:
                        break
                    assert time.monotonic() < deadline
                    time.sleep(0.02)
                assert b.call('PEXPIRETIME', 'a') == FUTURE
                assert b.call('PEXPIRETIME', 'b') == FUTURE
                assert a.call('MULTI') == b'OK'
                assert a.call('MSETEX', 2, 'a', 'x', 'b', 'y', 'XX',
                              'KEEPTTL') == b'QUEUED'
                assert a.call('MSETEX', 2, 'a', 'bad', 'absent', 'bad',
                              'XX') == b'QUEUED'
                assert a.call('EXEC') == [1, 0]
                deadline = time.monotonic() + 6
                while True:
                    current = b.call('MGET', 'a', 'b')
                    assert current in (values, [b'x', b'y'])
                    if current == [b'x', b'y']:
                        break
                    assert time.monotonic() < deadline
                    time.sleep(0.02)
                assert b.call('PEXPIRETIME', 'a') == FUTURE
                assert a.call('MSETEX', 2, 'expire-a', 'x', 'expire-b', 'y',
                              'PX', 2000) == 1
                deadline = time.monotonic() + 6
                while b.call('DBSIZE') != 5:
                    assert time.monotonic() < deadline
                    time.sleep(0.02)
                deadline = time.monotonic() + 6
                while a.call('DBSIZE') != 3 or b.call('DBSIZE') != 3:
                    assert time.monotonic() < deadline
                    time.sleep(0.02)
            finally:
                a.close()
                b.close()


def cluster(binary):
    with server(binary, cluster=True) as port:
        c = Client(port)
        try:
            assert c.call('MSETEX', 0, 'a', 'v') == ErrorReply(
                b'ERR invalid numkeys value')
            assert c.call('MSETEX', 2, '{a}k', 'v', '{b}k', 'v') == \
                ErrorReply(b"CROSSSLOT Keys in request don't hash "
                           b"to the same slot")
            keys = ['{same}a', '{same}b']
            slot = c.call('CLUSTER', 'KEYSLOT', keys[0])
            assert c.call('CLUSTER', 'ADDSLOTS', slot) == b'OK'
            assert c.call('MSETEX', 2, keys[0], '{other}v', keys[1], 'PX',
                          'PXAT', FUTURE) == 1
            assert c.call('MGET', *keys) == [b'{other}v', b'PX']
            assert c.call('MULTI') == b'OK'
            assert c.call('MSETEX', 2, keys[0], 'x', keys[1], 'y',
                          'XX', 'KEEPTTL') == b'QUEUED'
            assert c.call('MGET', *keys) == b'QUEUED'
            assert c.call('EXEC') == [1, [b'x', b'y']]
        finally:
            c.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--pkv', required=True)
    parser.add_argument('--redis', required=True)
    args = parser.parse_args()
    with server(args.redis, redis=True) as ref, server(args.pkv) as port:
        checks = exercise(port, ref)
        race(port)
        race(ref)
    replication(args.pkv)
    cluster(args.pkv)
    print('MSETEX differential assertions:', checks)


if __name__ == '__main__':
    main()
