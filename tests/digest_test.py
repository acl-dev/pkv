#!/usr/bin/env python3
"""Redis DIGEST, digest SET and DELEX differential and live delivery checks."""
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
        assert actual == expected, (args[:5], actual, expected)
        checks += 1
        return actual

    try:
        for length in [0, 1, 3, 8, 16, 17, 128, 129, 240, 241, 1024,
                       1025, 16383, 16384, 16385, 70000]:
            value = bytes(i % 251 for i in range(length))
            compare('SET', 'value', value, 'PXAT', 4102444800123)
            digest = compare('DIGEST', 'value')
            assert len(digest) == 16
            compare('PEXPIRETIME', 'value')
            for condition in ['IFDEQ', 'IFDNE']:
                for expected in [digest, digest.upper(), b'g' * 16,
                                 b'0' * 16]:
                    for get in [False, True]:
                        compare('SET', 'value', value, 'PXAT', 4102444800123)
                        compare('SET', 'value', b'new\0x', condition, expected,
                                'KEEPTTL', *(['GET'] if get else []))
                        compare('GET', 'value')
                        compare('PEXPIRETIME', 'value')
            for condition, expected in [('IFEQ', value), ('IFNE', value),
                                        ('IFDEQ', digest),
                                        ('IFDNE', digest),
                                        ('IFDEQ', digest.upper()),
                                        ('IFDNE', b'g' * 16)]:
                compare('SET', 'value', value, 'PXAT', 4102444800123)
                compare('DELEX', 'value', condition, expected)
                compare('TYPE', 'value')
        for value in ['0', '-1', '9223372036854775807',
                      '-9223372036854775808', '001', '1.0']:
            compare('SET', 'numeric', value)
            compare('DIGEST', 'numeric')
        compare('SET', 'numeric', '0')
        compare('INCR', 'numeric')
        compare('DIGEST', 'numeric')
        for seed in [('SET', 'typed', 'str'), ('LPUSH', 'typed', 'x'),
                     ('HSET', 'typed', 'x', 'v'), ('SADD', 'typed', 'x'),
                     ('ZADD', 'typed', '1', 'x')]:
            compare('DEL', 'typed')
            compare(*seed)
            if seed[0] != 'SET':
                compare('DIGEST', 'typed')
            for condition in ['IFDEQ', 'IFDNE']:
                for malformed in [b'', b'x', b'0' * 15, b'0' * 17]:
                    compare('SET', 'typed', 'new', condition, malformed)
            for condition in ['IFEQ', 'IFNE', 'IFDEQ', 'IFDNE', 'invalid']:
                compare('DELEX', 'typed', condition, '')
            compare('DELEX', 'typed')
            compare('TYPE', 'typed')
        for key in ['missing', 'expired']:
            compare('DEL', key)
            if key == 'expired':
                compare('SET', key, 'old', 'PXAT', 1)
            compare('DIGEST', key)
            compare('DELEX', key, 'unknown', 'short')
            for condition in ['IFDEQ', 'IFDNE']:
                compare('DEL', key)
                compare('SET', key, 'new', condition, 'short', 'GET')
                compare('GET', key)
                compare('DELEX', key)
        for args in [('DIGEST',), ('DIGEST', 'x', 'extra'), ('DELEX',),
                     ('DELEX', 'missing', 'IFEQ'),
                     ('DELEX', 'missing', 'IFEQ', 'x', 'extra')]:
            compare(*args)
        compare('SET', b'key\0x', b'value\xff\0')
        hash_value = compare('DIGEST', b'key\0x')
        compare('DELEX', b'key\0x', 'ifdeq', hash_value.upper())
        compare('SET', 'value', 'old')
        old = compare('DIGEST', 'value')
        compare('SET', 'hash-source', 'new')
        new = compare('DIGEST', 'hash-source')
        compare('MULTI')
        compare('SET', 'value', 'new', 'IFDEQ', old)
        compare('DIGEST', 'value')
        compare('DELEX', 'value', 'IFDNE', new)
        compare('DELEX', 'value', 'IFDEQ', new)
        compare('DIGEST', 'value')
        compare('EXEC')
        compare('SELECT', 1)
        compare('SET', 'value', 'db1', 'IFDNE', 'malformed')
        compare('DIGEST', 'value')
        compare('DELEX', 'value')
        compare('SELECT', 0)
        compare('DIGEST', 'value')
        for condition in [b'IFDEQ\0x', b'IFDNE\0x']:
            assert isinstance(a.call('SET', 'value', 'v', condition, old),
                              ErrorReply)
        for command in [('DIGEST', 'value'), ('DELEX', 'value'),
                        ('DELEX', 'value', 'IFDEQ', old),
                        ('DELEX', 'value', 'IFEQ'),
                        ('DELEX', 'value', 'invalid', 'IFEQ'),
                        ('DELEX', 'value', 'invalid', 'v')]:
            compare('COMMAND', 'GETKEYS', *command)
            compare('COMMAND', 'GETKEYSANDFLAGS', *command)
        for condition in ['IFDEQ', 'IFDNE']:
            assert a.call('SET', 'single-reply', 'old') == b'OK'
            assert a.call('SET', 'single-reply', 'new', condition,
                          'short', 'GET') == ErrorReply(
                              b'ERR must be exactly 16 hexadecimal characters')
            assert a.call('PING') == b'PONG'
            assert a.call('GET', 'single-reply') == b'old'
            assert a.call('MULTI') == b'OK'
            assert a.call('SET', 'single-reply', 'new', condition,
                          'short', 'GET') == b'QUEUED'
            assert a.call('GET', 'single-reply') == b'QUEUED'
            assert a.call('EXEC') == [ErrorReply(
                b'ERR must be exactly 16 hexadecimal characters'), b'old']
            assert a.call('PING') == b'PONG'
        watches(port)
        watches(reference)
        return checks
    finally:
        a.close()
        b.close()


def watches(port):
    a, b = Client(port), Client(port)
    try:
        assert a.call('SET', 'watch-digest', 'old') == b'OK'
        assert a.call('WATCH', 'watch-digest') == b'OK'
        old = b.call('DIGEST', 'watch-digest')
        assert b.call('DELEX', 'watch-digest', 'IFDNE', old) == 0
        assert a.call('MULTI') == b'OK'
        assert a.call('GET', 'watch-digest') == b'QUEUED'
        assert a.call('EXEC') == [b'old']
        assert a.call('WATCH', 'watch-digest') == b'OK'
        assert b.call('DELEX', 'watch-digest', 'IFDEQ', old) == 1
        assert a.call('MULTI') == b'OK'
        assert a.call('GET', 'watch-digest') == b'QUEUED'
        assert a.call('EXEC') is None
    finally:
        a.close()
        b.close()


def race(port):
    c = Client(port)
    try:
        assert c.call('SET', 'delete-race', b'x' * 17000) == b'OK'
        digest = c.call('DIGEST', 'delete-race')
    finally:
        c.close()

    def worker(_):
        client = Client(port)
        try:
            return client.call('DELEX', 'delete-race', 'IFDEQ', digest)
        finally:
            client.close()

    with ThreadPoolExecutor(max_workers=8) as pool:
        results = list(pool.map(worker, range(8)))
    assert results.count(1) == 1 and results.count(0) == 7, results


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
                assert a.call('SET', 'copy', b'a' * 70000) == b'OK'
                old = a.call('DIGEST', 'copy')
                deadline = time.monotonic() + 6
                while b.call('DIGEST', 'copy') != old:
                    assert time.monotonic() < deadline
                    time.sleep(0.02)
                assert a.call('MULTI') == b'OK'
                assert a.call('SET', 'copy', b'b' * 40000, 'IFDEQ', old) == \
                    b'QUEUED'
                assert a.call('DELEX', 'copy', 'IFNE', b'b' * 40000) == \
                    b'QUEUED'
                assert a.call('EXEC') == [b'OK', 0]
                new = a.call('DIGEST', 'copy')
                deadline = time.monotonic() + 6
                while b.call('DIGEST', 'copy') != new:
                    assert time.monotonic() < deadline
                    time.sleep(0.02)
                assert a.call('DELEX', 'copy', 'IFDEQ', new) == 1
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
            key = '{served}key'
            assert isinstance(c.call('DIGEST', key), ErrorReply)
            slot = c.call('CLUSTER', 'KEYSLOT', key)
            assert c.call('CLUSTER', 'ADDSLOTS', slot) == b'OK'
            assert c.call('SET', key, '{other}value') == b'OK'
            assert len(c.call('DIGEST', key)) == 16
            assert c.call('DELEX', key, 'IFEQ', '{other}value') == 1
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
    print('DIGEST/DELEX differential assertions:', checks)


if __name__ == '__main__':
    main()
