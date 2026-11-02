#!/usr/bin/env python3
"""Float increments: byte-exact Redis comparisons and atomicity checks."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import os
import random
import threading

from integration_test import Client, ErrorReply, server


def run(port):
    client = Client(port)
    checks = 0

    def check(expected, *args):
        nonlocal checks
        actual = client.call(*args)
        assert type(actual) is type(expected) and actual == expected, (
            args, actual, expected)
        checks += 1

    def error(message, *args):
        check(ErrorReply(message), *args)

    field = b'f\0' + bytes(range(256))
    wrongtype = (b'WRONGTYPE Operation against a key holding the wrong '
                 b'kind of value')
    try:
        for command, key, middle in [('INCRBYFLOAT', b'float\0s', ()),
            ('HINCRBYFLOAT', b'float\0h', (field,))]:
            check(b'0.125', command, key, *middle, '.125')
            check(1, 'PEXPIREAT', key, 4102444800123)
            check(b'0.25', command, key, *middle, '+.125')
            check(b'0', command, key, *middle, '-.25')
            check(b'0', command, key, *middle, '-0')
            check(b'125', command, key, *middle, '1.25e2')
            check(b'126', command, key, *middle, '0x1p0')
            check(4102444800123, 'PEXPIRETIME', key)
            for invalid in ('', 'nan', 'NaN(foo)', '1 ', '\t1', '1,5',
                            '1e99999', '1e-99999', b'1\0'):
                error(b'ERR value is not a valid float', command, key,
                      *middle, invalid)
                check(4102444800123, 'PEXPIRETIME', key)
            check(b'126', 'GET' if not middle else 'HGET', key, *middle)
            check(1, 'PEXPIREAT', key, 1)
            check(b'2', command, key, *middle, '2')
            check(-1, 'PTTL', key)
        check(b'0', 'INCRBYFLOAT', b'', '-0')
        check(1, 'DEL', b'')
        check(b'0', 'HINCRBYFLOAT', b'', b'', '0')
        check(b'OK', 'SET', 'float-wrong-hash', 'string')
        error(b'ERR value is not a valid float', 'HINCRBYFLOAT',
              'float-wrong-hash', 'field', 'bad')
        error(b'ERR value is NaN or Infinity', 'HINCRBYFLOAT',
              'float-wrong-hash', 'field', 'inf')
        error(wrongtype,
              'HINCRBYFLOAT', 'float-wrong-hash', 'field', '1')
        check(1, 'HSET', 'float-wrong-string', 'field', 'value')
        error(wrongtype,
              'INCRBYFLOAT', 'float-wrong-string', 'bad')
        for command, key, middle in [('INCRBYFLOAT', 'float-infinite-s', ()),
            ('HINCRBYFLOAT', 'float-infinite-h', ('field',))]:
            error(b'ERR increment would produce NaN or Infinity'
                  if not middle else b'ERR value is NaN or Infinity',
                  command, key, *middle, 'Infinity')
            check(0, 'EXISTS', key)
            if not middle:
                check(b'OK', 'SET', key, '-inf')
            else:
                check(1, 'HSET', key, *middle, '-inf')
            error(b'ERR increment would produce NaN or Infinity',
                  command, key, *middle, '1')
            check(b'-inf', 'GET' if not middle else 'HGET', key, *middle)
        for args in [('INCRBYFLOAT',), ('INCRBYFLOAT', 'key'),
            ('INCRBYFLOAT', 'key', 1, 2), ('HINCRBYFLOAT',),
            ('HINCRBYFLOAT', 'key', 'field'),
            ('HINCRBYFLOAT', 'key', 'field', 1, 2)]:
            actual = client.call(*args)
            assert isinstance(actual, ErrorReply) and actual.startswith(
                b'ERR wrong number of arguments')
            checks += 1
        commands = [('HINCRBYFLOAT', 'float-pipeline', 'field', 'bad'),
                    ('HINCRBYFLOAT', 'float-pipeline', 'field', '.125'),
                    ('INCRBYFLOAT', 'float-pipeline-s', '.125'), ('PING',)]
        client.socket.sendall(b''.join(client.encode(args)
                                      for args in commands))
        assert isinstance(client.read(), ErrorReply)
        assert [client.read() for _ in range(3)] == [b'0.125', b'0.125',
                                                  b'PONG']
        checks += 4
    finally:
        client.close()
    start = threading.Barrier(8)

    def worker(_):
        connection = Client(port)
        try:
            start.wait(timeout=10)
            hashes, strings = [], []
            for _ in range(50):
                hashes.append(connection.call('HINCRBYFLOAT', 'float-race-h',
                                              field, '.125'))
                strings.append(connection.call('INCRBYFLOAT', 'float-race-s',
                                               '.125'))
            return hashes, strings
        finally:
            connection.close()

    with ThreadPoolExecutor(max_workers=8) as pool:
        results = list(pool.map(worker, range(8)))
    expected = [i / 8 for i in range(1, 401)]
    for column in (0, 1):
        actual = [value for result in results for value in result[column]]
        assert all(type(value) is bytes for value in actual)
        assert sorted(float(value) for value in actual) == expected
        checks += 400
    return checks


def compare(first, second):
    candidate, reference = Client(first), Client(second)
    randomizer = random.Random(208712)
    cases = [(old, increment) for old in ('0', '-0', '1.25e2', '0x1.4p1',
        '0.1', 'NaN', 'inf', '-inf', '', ' 1', b'1\0', '0' * 5119,
        '0' * 5120)
        for increment in ('.1', '-0.1', 'inf', '-inf', 'bad', '1e99999')]
    for _ in range(200):
        cases.append(('%de%d' % (randomizer.randrange(-100000, 100000),
                                 randomizer.randrange(-25, 26)),
                      '%de%d' % (randomizer.randrange(-100000, 100000),
                                 randomizer.randrange(-25, 26))))
    checks = 0
    try:
        for old, increment in cases:
            for command, key, middle in [('INCRBYFLOAT', 'float-diff-s', ()),
                ('HINCRBYFLOAT', 'float-diff-h', ('field',))]:
                replies, stored = [], []
                for client in (candidate, reference):
                    client.call('SET' if not middle else 'HSET', key,
                                *middle, old)
                    replies.append(client.call(command, key, *middle,
                                               increment))
                    stored.append(client.call('GET' if not middle else
                                              'HGET', key, *middle))
                assert type(replies[0]) is type(replies[1]) and \
                    replies[0] == replies[1] and stored[0] == stored[1], (
                        command, old, increment, replies, stored)
                checks += 1
    finally:
        candidate.close()
        reference.close()
    return checks


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--pkv', required=True)
    parser.add_argument('--redis')
    args = parser.parse_args()
    with server(os.path.abspath(args.pkv)) as port:
        print('pkv: %d float increment checks passed' % run(port))
        if args.redis:
            with server(os.path.abspath(args.redis), True) as reference:
                print('Redis: %d float increment checks passed' %
                      run(reference))
                print('pkv/Redis: %d float increment comparisons passed' %
                      compare(port, reference))


if __name__ == '__main__':
    main()
