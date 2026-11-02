#!/usr/bin/env python3
"""BITFIELD: arbitrary-precision model, protocol errors and atomic replies."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import os
import random
import threading

from integration_test import Client, ErrorReply, server

MINIMUM, MAXIMUM = -(1 << 63), (1 << 63) - 1


def model(value, operations):
    data = bytearray(value or b'')
    writes = [op for op in operations if op[0] != 'GET']
    if writes:
        length = max((offset + bits + 7) // 8
                     for _, _, bits, offset, _, _ in writes)
        data.extend(b'\0' * max(0, length - len(data)))
    replies = []
    for command, sign, bits, offset, argument, overflow in operations:
        start, skip = offset // 8, offset % 8
        count = (skip + bits + 7) // 8
        chunk = int.from_bytes(data[start:start + count].ljust(count, b'\0'),
                               'big')
        shift = count * 8 - skip - bits
        mask = (1 << bits) - 1
        old = (chunk >> shift) & mask
        if sign and old >> (bits - 1):
            old -= 1 << bits
        if command == 'GET':
            replies.append(old)
            continue
        minimum = -(1 << (bits - 1)) if sign else 0
        maximum = (1 << (bits - int(sign))) - 1
        candidate = argument if command == 'SET' else old + argument
        # Unsigned SET first converts its signed argument to uint64_t.
        if command == 'SET' and not sign:
            candidate %= 1 << 64
        invalid = candidate < minimum or candidate > maximum
        if invalid and overflow == 'FAIL':
            replies.append(None)
            continue
        if invalid and overflow == 'SAT':
            # Redis's signed SET checks wrapped max-delta before min bounds.
            upper = command == 'SET' and sign and bits != 64 and \
                (maximum - argument) % (1 << 64) > MAXIMUM
            candidate = maximum if upper else min(maximum,
                                                   max(minimum, candidate))
        raw = candidate & mask
        new = raw - (1 << bits) if sign and raw >> (bits - 1) else raw
        replies.append(old if command == 'SET' else new)
        chunk = (chunk & ~(mask << shift)) | (raw << shift)
        data[start:start + count] = chunk.to_bytes(count, 'big')
    return replies, bytes(data) if writes or value is not None else None


def arguments(operations):
    result = []
    for command, sign, bits, offset, value, overflow in operations:
        result += ['OVERFLOW', overflow, command,
                   ('i' if sign else 'u') + str(bits), offset]
        if command != 'GET':
            result.append(value)
    return result


def generated(seed, count):
    randomizer = random.Random(seed)
    for _ in range(count):
        value = bytes(randomizer.randrange(256)
                      for _ in range(randomizer.choice([0, 1, 9, 1025, 16385])))
        operations = []
        for _ in range(6):
            sign = bool(randomizer.randrange(2))
            bits = randomizer.randint(1, 64 if sign else 63)
            operations.append((randomizer.choice(['GET', 'SET', 'INCRBY']),
                sign, bits, randomizer.choice([0, 1, 7, 8, 8191, 131071]),
                randomizer.choice([MINIMUM, MAXIMUM, -1, 0, 1, 255, -256,
                    randomizer.randrange(MINIMUM, MAXIMUM + 1)]),
                randomizer.choice(['WRAP', 'SAT', 'FAIL'])))
        yield value, operations


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

    key = b'field\0' + bytes(range(256))
    type_error = (b'ERR Invalid bitfield type. Use something like i16 u8. '
                  b'Note that u64 is not supported but i64 is.')
    wrongtype = (b'WRONGTYPE Operation against a key holding the wrong '
                 b'kind of value')
    try:
        check([], 'BITFIELD', key)
        check([], 'BITFIELD_RO', key, 'OVERFLOW', 'FAIL')
        check([0, 0], 'BITFIELD_RO', key, 'GET', 'i64', 4294967295,
              'GET', 'u63', '#1')
        check(0, 'EXISTS', key)
        check([None, 0], 'BITFIELD', key, 'OVERFLOW', 'FAIL',
              'SET', 'u1', '#100', 2, 'GET', 'u1', 100)
        check(b'\0' * 13, 'GET', key)
        check(-1, 'PTTL', key)
        check([0, -1, -1, MAXIMUM, MINIMUM], 'BITFIELD', key,
              'SET', 'i64', 3, -1, 'GET', 'i64', 3,
              'SET', 'i64', 3, MAXIMUM, 'GET', 'i64', 3,
              'INCRBY', 'i64', 3, 1)
        check([MINIMUM, MINIMUM, None, MINIMUM], 'BITFIELD', key,
              'OVERFLOW', 'SAT', 'INCRBY', 'i64', 3, -1,
              'GET', 'i64', 3, 'OVERFLOW', 'FAIL',
              'INCRBY', 'i64', 3, -1, 'GET', 'i64', 3)
        check(b'OK', 'SET', key, b'\0')
        check([0, 255, 255, 0], 'BITFIELD', key,
              'OVERFLOW', 'SAT', 'SET', 'u8', 0, -1,
              'GET', 'u8', 0, 'OVERFLOW', 'WRAP',
              'SET', 'u8', 0, -256, 'GET', 'u8', 0)
        check([0], 'BITFIELD_RO', key, 'GET', b'u8\0ignored', 0)
        check([0, 15], 'BITFIELD', key, 'OVERFLOW', 'SAT',
              'SET', 'i5', 0, MINIMUM, 'GET', 'i5', 0)
        check(b'OK', 'SET', key, b'\0')
        check(1, 'PEXPIREAT', key, 4102444800123)
        check(1, 'HSET', 'field-wrong', 'f', 'v')
        for command in ('BITFIELD', 'BITFIELD_RO'):
            error(wrongtype, command, 'field-wrong')
            error(wrongtype, command, 'field-wrong', 'GET', 'i1', 0)
            for kind in ('u64', 'i65', 'u0', 'i0', 'I8', 'u01', 'u+1', ''):
                error(type_error, command, 'field-wrong', 'GET', kind, 'bad')
            for offset in ('bad', -1, '#-1', 4294967296, '#536870912',
                           '01', '+1', b'1\0'):
                error(b'ERR bit offset is not an integer or out of range',
                      command, key, 'GET', 'u8', offset)
            error(b'ERR Invalid OVERFLOW type specified',
                  command, key, 'OVERFLOW', 'bad')
            for tail in [('GET',), ('SET', 'u8', 0), ('OVERFLOW',),
                         ('bad', 'u8', 0)]:
                error(b'ERR syntax error', command, key, *tail)
        error(b'ERR BITFIELD_RO only supports the GET subcommand',
              'BITFIELD_RO', 'field-wrong', 'SET', 'u8', 0, 1)
        error(type_error, 'BITFIELD_RO', key, 'SET', 'u8', 0, 1,
              'GET', 'u64', 0)
        error(b'ERR value is not an integer or out of range',
              'BITFIELD_RO', key, 'SET', 'u8', 0, 'bad')
        error(b'ERR syntax error', 'BITFIELD', key, 'SET', 'u8', 0, 10, 'bad')
        check(b'\0', 'GET', key)
        check(4102444800123, 'PEXPIRETIME', key)
        check(1, 'PEXPIREAT', 'field-wrong', 1)
        check([0], 'BITFIELD_RO', 'field-wrong', 'GET', 'i64', 0)
        for value, operations in generated(671240, 200):
            expected, stored = model(value, operations)
            check(b'OK', 'SET', key, value, 'PXAT', 4102444800123)
            check(expected, 'BITFIELD', key, *arguments(operations))
            check(stored, 'GET', key)
            check(4102444800123, 'PEXPIRETIME', key)
        commands = [('BITFIELD', key, 'OVERFLOW', 'FAIL', 'SET', 'u1', 0, 2),
                    ('BITFIELD_RO', key), ('PING',)]
        client.socket.sendall(b''.join(client.encode(args)
                                      for args in commands))
        assert [client.read() for _ in commands] == [[None], [], b'PONG']
        checks += 3
    finally:
        client.close()
    return checks + concurrent(port)


def concurrent(port):
    client = Client(port)
    try:
        assert client.call('SET', 'field-race', b'\0' * 20001,
                           'PXAT', 4102444800123) == b'OK'
    finally:
        client.close()
    barrier = threading.Barrier(8)

    def worker(_):
        connection = Client(port)
        try:
            barrier.wait(timeout=10)
            result = []
            for _ in range(50):
                reply = connection.call('BITFIELD', 'field-race',
                    'INCRBY', 'i64', 16384 * 8 - 1, 1)
                assert isinstance(reply, list) and len(reply) == 1
                assert type(reply[0]) is int
                result.append(reply[0])
            return result
        finally:
            connection.close()
    with ThreadPoolExecutor(max_workers=8) as executor:
        values = [value for batch in executor.map(worker, range(8))
                  for value in batch]
    assert sorted(values) == list(range(1, 401))
    client = Client(port)
    try:
        assert client.call('BITFIELD_RO', 'field-race', 'GET',
                           'i64', 16384 * 8 - 1) == [400]
        assert client.call('PEXPIRETIME', 'field-race') == 4102444800123
    finally:
        client.close()
    return 403


def compare(first, second):
    clients = Client(first), Client(second)
    checks = 0
    try:
        for value, operations in generated(641111, 200):
            results = []
            for client in clients:
                assert client.call('SET', 'field-diff', value) == b'OK'
                results.append((client.call('BITFIELD', 'field-diff',
                                            *arguments(operations)),
                                client.call('GET', 'field-diff')))
            assert results[0] == results[1], (value[:32], operations, results)
            checks += 1
    finally:
        for client in clients:
            client.close()
    return checks


def limits(port):
    client = Client(port)
    try:
        assert client.call('SET', 'field-limit', 'safe') == b'OK'
        assert client.call('BITFIELD_RO', 'field-limit', 'GET', 'u8',
            '#2305843009213693952') == ErrorReply(
                b'ERR bit offset is not an integer or out of range')
        assert client.call('BITFIELD_RO', 'field-limit', 'GET',
                           'i64', 4294967295) == [0]
        assert client.call('GET', 'field-limit') == b'safe'
    finally:
        client.close()
    return 4


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--pkv', required=True)
    parser.add_argument('--redis')
    args = parser.parse_args()
    with server(os.path.abspath(args.pkv)) as port:
        print('pkv: %d BITFIELD checks passed' % run(port))
        print('pkv: %d bounded-offset checks passed' % limits(port))
        if args.redis:
            with server(os.path.abspath(args.redis), True) as reference:
                print('Redis: %d BITFIELD checks passed' % run(reference))
                print('pkv/Redis: %d BITFIELD comparisons passed' %
                      compare(port, reference))


if __name__ == '__main__':
    main()
