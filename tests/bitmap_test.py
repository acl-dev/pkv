#!/usr/bin/env python3
"""Native bitmap commands: independent model, Redis replies and byte races."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import os
import random
import threading

from integration_test import Client, ErrorReply, server


def bounds(length, start, end, bits, count):
    size = length * (8 if bits else 1)
    if not size or (count and start < 0 and end < 0 and start > end):
        return None
    start = max(0, size + start if start < 0 else start)
    end = min(size - 1, max(0, size + end if end < 0 else end))
    if start > end:
        return None
    return (start, end) if bits else (start * 8, end * 8 + 7)


def count_model(value, start=0, end=-1, bits=False):
    window = bounds(len(value), start, end, bits, True)
    if window is None:
        return 0
    first, last = window
    return sum((value[n // 8] >> (7 - n % 8)) & 1
               for n in range(first, last + 1))


def position_model(value, bit, start=0, end=None, bits=False):
    if value is None:
        return -1 if bit else 0
    window = bounds(len(value), start, -1 if end is None else end,
                    bits, False)
    if window is None:
        return -1
    first, last = window
    for n in range(first, last + 1):
        if ((value[n // 8] >> (7 - n % 8)) & 1) == bit:
            return n
    return len(value) * 8 if bit == 0 and end is None else -1


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

    key = b'bitmap\0' + bytes(range(256))
    wrongtype = (b'WRONGTYPE Operation against a key holding the wrong '
                 b'kind of value')
    try:
        check(0, 'GETBIT', key, 4294967295)
        check(0, 'BITCOUNT', key)
        for bit in (0, 1):
            for tail in ((), (100,), (100, -100), (2, 1, 'BIT')):
                check(-1 if bit else 0, 'BITPOS', key, bit, *tail)
        check(0, 'EXISTS', key)
        check(b'OK', 'SET', key, b'')
        check(-1, 'BITPOS', key, 0)
        check(-1, 'BITPOS', key, 0, 0, -1, 'BIT')
        check(0, 'SETBIT', key, 7, 0)
        check(b'\0', 'GET', key)
        check(0, 'SETBIT', key, 0, 1)
        check(1, 'SETBIT', key, 0, 1)
        check(1, 'SETBIT', key, 0, 0)
        check(b'OK', 'SET', key, b'\xff\xff')
        check(16, 'BITPOS', key, 0)
        check(16, 'BITPOS', key, 0, 1)
        check(-1, 'BITPOS', key, 0, 2)
        check(-1, 'BITPOS', key, 0, 0, -1)
        check(-1, 'BITPOS', key, 0, 7, 8, 'BIT')
        check(1, 'PEXPIREAT', key, 4102444800123)
        check(0, 'SETBIT', key, 16384 * 8, 1)
        check(1, 'GETBIT', key, 16384 * 8)
        check(4102444800123, 'PEXPIRETIME', key)
        check(b'\0\x80', 'GETRANGE', key, 16383, 16384)
        check(1, 'PEXPIREAT', key, 1)
        check(0, 'BITCOUNT', key)
        check(0, 'GETBIT', key, 0)
        check(0, 'SETBIT', key, 9, 0)
        check(b'\0\0', 'GET', key)
        check(-1, 'PTTL', key)
        check(1, 'HSET', 'bitmap-wrong', 'f', 'v')
        for args in [('GETBIT', 'bitmap-wrong', 0),
            ('SETBIT', 'bitmap-wrong', 0, 1), ('BITCOUNT', 'bitmap-wrong'),
            ('BITPOS', 'bitmap-wrong', 0, 20, 0, 'BIT')]:
            error(wrongtype, *args)
        for offset in (-1, 4294967296, '01', '+1', '1 ', 'bad', b'1\0',
                       '9223372036854775808'):
            for command, tail in [('GETBIT', ()), ('SETBIT', ('bad',))]:
                error(b'ERR bit offset is not an integer or out of range',
                      command, 'bitmap-wrong', offset, *tail)
        for bit in (-1, 2, 'bad', '01', '+1', b'1\0'):
            error(b'ERR bit is not an integer or out of range',
                  'SETBIT', 'bitmap-wrong', 0, bit)
        error(b'ERR value is not an integer or out of range',
              'BITPOS', 'bitmap-wrong', 'bad', 0, 'bad', 'bad')
        error(b'ERR The bit argument must be 1 or 0.',
              'BITPOS', 'bitmap-wrong', 2)
        for args in [('BITCOUNT', key, 0), ('BITCOUNT', key, 0, 1, 'bad'),
            ('BITCOUNT', key, 0, 1, 'BIT', 'extra'),
            ('BITPOS', key, 0, 0, 'bad', 'bad'),
            ('BITPOS', key, 0, 0, 1, 'BIT', 'extra')]:
            error(b'ERR syntax error', *args)
        for args in [('BITCOUNT', 'bitmap-wrong', 'bad', 0),
            ('BITPOS', 'bitmap-wrong', 0, 'bad'),
            ('BITPOS', 'bitmap-wrong', 0, 0, 'bad', 'BIT')]:
            error(b'ERR value is not an integer or out of range', *args)
        for args in [('GETBIT', key), ('SETBIT', key, 1), ('BITCOUNT',),
                     ('BITPOS', key)]:
            actual = client.call(*args)
            assert isinstance(actual, ErrorReply) and actual.startswith(
                b'ERR wrong number of arguments'), (args, actual)
            checks += 1
        randomizer = random.Random(221031)
        expected = bytearray()
        check(1, 'DEL', key)
        for i in range(160):
            offset = randomizer.randrange(40000 * 8)
            bit = randomizer.randrange(2)
            byte, shift = offset // 8, 7 - offset % 8
            previous = (expected[byte] >> shift) & 1 \
                if byte < len(expected) else 0
            check(previous, 'SETBIT', key, offset, bit)
            expected.extend(b'\0' * max(0, byte + 1 - len(expected)))
            expected[byte] = ((expected[byte] & ~(1 << shift)) |
                              (bit << shift))
            check(bit, 'GETBIT', key, offset)
            if i % 40 == 0:
                check(bytes(expected), 'GET', key)
        check(bytes(expected), 'GET', key)
        value = b'\xff' * 16384 + bytes(range(256)) * 80 + b'\0' * 1024
        check(b'OK', 'SET', key, value)
        indexes = [-9223372036854775808, -400001, -2, -1, 0, 1, 7,
                   8, 16384, 131071, 131072, 9223372036854775807]
        for i in range(140):
            start, end = randomizer.choice(indexes), randomizer.choice(indexes)
            bits = bool(i % 2)
            unit = 'BIT' if bits else 'BYTE'
            check(count_model(value, start, end, bits),
                  'BITCOUNT', key, start, end, unit)
            for bit in (0, 1):
                check(position_model(value, bit, start, end, bits),
                      'BITPOS', key, bit, start, end, unit)
        check(count_model(value), 'BITCOUNT', key)
        commands = [('SETBIT', key, 0, 0), ('GETBIT', key, 0),
                    ('BITCOUNT', key, 0, 0, 'BIT'), ('PING',)]
        client.socket.sendall(b''.join(client.encode(args)
                                      for args in commands))
        assert [client.read() for _ in commands] == [1, 0, 0, b'PONG']
        checks += 4
    finally:
        client.close()
    return checks + concurrent(port)


def concurrent(port):
    client = Client(port)
    try:
        assert client.call('SET', 'bitmap-race', b'\0' * 200) == b'OK'
    finally:
        client.close()
    barrier = threading.Barrier(8)

    def worker(thread):
        connection = Client(port)
        try:
            barrier.wait(timeout=10)
            return [connection.call('SETBIT', 'bitmap-race',
                                    i * 8 + thread, 1) for i in range(100)]
        finally:
            connection.close()
    with ThreadPoolExecutor(max_workers=8) as executor:
        results = list(executor.map(worker, range(8)))
    assert all(type(bit) is int and bit == 0
               for result in results for bit in result)
    client = Client(port)
    try:
        assert client.call('GET', 'bitmap-race') == b'\xff' * 100 + b'\0' * 100
        assert client.call('BITCOUNT', 'bitmap-race') == 800
    finally:
        client.close()
    return 803


def compare(first, second):
    clients = Client(first), Client(second)
    randomizer = random.Random(81331)
    checks = 0
    try:
        for value in (b'', b'\0', b'\xff', b'\xff\0\x81',
                      bytes(range(256)), b'\xff' * 16385):
            for client in clients:
                assert client.call('SET', 'bitmap-diff', value) == b'OK'
            commands = [('BITPOS', 'bitmap-diff', 0),
                        ('BITPOS', 'bitmap-diff', 1)]
            for _ in range(100):
                start, end = (randomizer.randrange(-len(value) * 9 - 5,
                    len(value) * 9 + 6) for _ in range(2))
                unit = randomizer.choice(['BIT', 'BYTE'])
                commands += [('BITCOUNT', 'bitmap-diff', start, end, unit),
                    ('BITPOS', 'bitmap-diff', randomizer.randrange(2),
                     start, end, unit),
                    ('BITPOS', 'bitmap-diff', 0, start)]
            for command in commands:
                replies = [client.call(*command) for client in clients]
                assert type(replies[0]) is type(replies[1]) and \
                    replies[0] == replies[1], (value[:32], command, replies)
                checks += 1
    finally:
        for client in clients:
            client.close()
    return checks


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--pkv', required=True)
    parser.add_argument('--redis')
    args = parser.parse_args()
    with server(os.path.abspath(args.pkv)) as port:
        print('pkv: %d bitmap checks passed' % run(port))
        if args.redis:
            with server(os.path.abspath(args.redis), True) as reference:
                print('Redis: %d bitmap checks passed' % run(reference))
                print('pkv/Redis: %d bitmap comparisons passed' %
                      compare(port, reference))


if __name__ == '__main__':
    main()
