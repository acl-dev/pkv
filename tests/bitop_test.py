#!/usr/bin/env python3
"""BITOP: all operators, aliases, key snapshots and Redis comparisons."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import functools
import operator
import os
import random
import threading

from integration_test import Client, ErrorReply, server

OPERATIONS = ('AND', 'OR', 'XOR', 'NOT', 'DIFF', 'DIFF1', 'ANDOR', 'ONE')


def model(operation, values):
    size = max(map(len, values))
    if not size:
        return b''
    numbers = [int.from_bytes(value.ljust(size, b'\0'), 'big')
               for value in values]
    mask = (1 << (size * 8)) - 1
    first = numbers[0]
    rest = functools.reduce(operator.or_, numbers[1:], 0)
    if operation == 'AND':
        result = functools.reduce(operator.and_, numbers)
    elif operation == 'OR':
        result = first | rest
    elif operation == 'XOR':
        result = functools.reduce(operator.xor, numbers)
    elif operation == 'NOT':
        result = ~first
    elif operation == 'DIFF':
        result = first & ~rest
    elif operation == 'DIFF1':
        result = ~first & rest
    elif operation == 'ANDOR':
        result = first & rest
    else:
        seen, repeated = 0, 0
        for value in numbers:
            repeated |= seen & value
            seen |= value
        result = seen & ~repeated
    return (result & mask).to_bytes(size, 'big')


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

    wrongtype = (b'WRONGTYPE Operation against a key holding the wrong '
                 b'kind of value')
    a, b, destination = b'bitop\0a', bytes(range(256)), b''
    try:
        check(b'OK', 'MSET', a, b'\x55', b, b'\xaa\xff')
        for operation in OPERATIONS:
            inputs = [a] if operation == 'NOT' else [a, b, a]
            values = [b'\x55'] if operation == 'NOT' else [
                b'\x55', b'\xaa\xff', b'\x55']
            expected = model(operation, values)
            check(len(expected), 'BITOP', operation.lower(),
                  destination, *inputs)
            check(expected, 'GET', destination)
            check(b'string', 'TYPE', destination)
        check(1, 'PEXPIREAT', a, 4102444800123)
        check(1, 'PEXPIREAT', b, 4102444800123)
        check(2, 'BITOP', 'XOR', destination, a, b, b)
        check(b'\x55\0', 'GET', destination)
        check(4102444800123, 'PEXPIRETIME', a)
        check(4102444800123, 'PEXPIRETIME', b)
        check(1, 'BITOP', 'NOT', a, a)
        check(b'\xaa', 'GET', a)
        check(-1, 'PTTL', a)
        check(1, 'HSET', 'bitop-hash', 'f', 'v')
        check(1, 'BITOP', 'OR', 'bitop-hash', a)
        check(b'\xaa', 'GET', 'bitop-hash')
        check(1, 'HSET', 'bitop-wrong', 'f', 'v')
        check(1, 'PEXPIREAT', a, 4102444800123)
        error(wrongtype, 'BITOP', 'AND', a, 'bitop-missing', 'bitop-wrong')
        check(b'\xaa', 'GET', a)
        check(4102444800123, 'PEXPIRETIME', a)
        error(b'ERR syntax error', 'BITOP', 'bad', a, 'bitop-wrong')
        error(b'ERR BITOP NOT must be called with a single source key.',
              'BITOP', 'NOT', a, 'bitop-wrong', a)
        for operation in ('DIFF', 'DIFF1', 'ANDOR'):
            error(('ERR BITOP %s must be called with at least two source '
                   'keys.' % operation).encode(),
                  'BITOP', operation, a, 'bitop-wrong')
        for args in [('BITOP',), ('BITOP', 'OR', a), ('BITOP', 'NOT')]:
            actual = client.call(*args)
            assert isinstance(actual, ErrorReply) and actual.startswith(
                b'ERR wrong number of arguments'), (args, actual)
            checks += 1
        check(1, 'PEXPIREAT', 'bitop-wrong', 1)
        check(1, 'BITOP', 'OR', destination, a, 'bitop-wrong')
        check(b'\xaa', 'GET', destination)
        check(b'OK', 'SET', 'bitop-empty', b'')
        check(0, 'BITOP', 'ONE', a, 'bitop-empty', 'bitop-missing')
        check(None, 'GET', a)
        check(0, 'BITOP', 'NOT', a, 'bitop-missing')
        check(0, 'EXISTS', a)
        randomizer = random.Random(81216)
        for step in range(64):
            operation = OPERATIONS[step % len(OPERATIONS)]
            lengths = [0, 1, 7, 8, 9, 1024, 1025, 16383, 16384, 16385, 40003]
            values = [bytes(randomizer.randrange(256)
                for _ in range(randomizer.choice(lengths))) for _ in range(3)]
            check(b'OK', 'MSET', a, values[0], b, values[1],
                  'bitop-third', values[2])
            names = [a, b, 'bitop-third']
            indexes = [randomizer.randrange(3) for _ in range(
                1 if operation == 'NOT' else 2 + step % 3)]
            inputs = [names[n] for n in indexes]
            target = inputs[step % len(inputs)] if step % 2 else destination
            expected = model(operation, [values[n] for n in indexes])
            check(len(expected), 'BITOP', operation, target, *inputs)
            check(expected if expected else None, 'GET', target)
        check(b'OK', 'SET', b, b'\xff' * 1025)
        commands = [('BITOP', 'NOT', 'bitop-pipeline', b),
                    ('BITOP', 'XOR', 'bitop-pipeline',
                     'bitop-pipeline', 'bitop-pipeline'),
                    ('BITCOUNT', 'bitop-pipeline'), ('PING',)]
        client.socket.sendall(b''.join(client.encode(args)
                                      for args in commands))
        replies = [client.read() for _ in commands]
        assert replies == [1025, 1025, 0, b'PONG'], replies
        checks += 4
    finally:
        client.close()
    return checks + concurrent(port)


def concurrent(port):
    client = Client(port)
    try:
        assert client.call('MSET', 'bitop-race', b'\0' * 1025,
                           'bitop-mask', b'\xff' * 1025) == b'OK'
    finally:
        client.close()
    barrier = threading.Barrier(6)

    def toggle(_):
        connection = Client(port)
        try:
            barrier.wait(timeout=10)
            for _ in range(40):
                assert connection.call('BITOP', 'XOR', 'bitop-race',
                    'bitop-race', 'bitop-mask') == 1025
        finally:
            connection.close()
    with ThreadPoolExecutor(max_workers=6) as executor:
        list(executor.map(toggle, range(6)))
    client = Client(port)
    try:
        assert client.call('GET', 'bitop-race') == b'\0' * 1025
        assert client.call('MSET', 'bitop-left', b'\0' * 1025,
                           'bitop-right', b'\0' * 1025) == b'OK'
    finally:
        client.close()
    barrier = threading.Barrier(4)

    def snapshot(worker):
        connection = Client(port)
        try:
            barrier.wait(timeout=10)
            for i in range(100):
                if worker == 0:
                    value = bytes([i % 256]) * 1025
                    assert connection.call('MSET', 'bitop-left', value,
                                           'bitop-right', value) == b'OK'
                else:
                    target = 'bitop-snapshot-%d' % worker
                    assert connection.call('BITOP', 'XOR', target,
                        'bitop-left', 'bitop-right') == 1025
                    assert connection.call('GET', target) == b'\0' * 1025
        finally:
            connection.close()
    with ThreadPoolExecutor(max_workers=4) as executor:
        list(executor.map(snapshot, range(4)))
    return 943


def compare(first, second):
    clients = Client(first), Client(second)
    randomizer = random.Random(82021)
    checks = 0
    try:
        for step in range(48):
            lengths = [0, 1, 7, 8, 9, 1024, 1025, 16385]
            values = [bytes(randomizer.randrange(256)
                for _ in range(randomizer.choice(lengths))) for _ in range(3)]
            names = ['bitop-diff-a', 'bitop-diff-b', 'bitop-diff-c']
            for operation in OPERATIONS:
                inputs = [randomizer.choice(names) for _ in range(
                    1 if operation == 'NOT' else 2 + step % 3)]
                target = inputs[step % len(inputs)] if step % 2 else 'bitop-out'
                results = []
                for client in clients:
                    assert client.call('MSET', names[0], values[0],
                        names[1], values[1], names[2], values[2]) == b'OK'
                    results.append((client.call('BITOP', operation, target,
                                                *inputs),
                                    client.call('GET', target),
                                    client.call('PTTL', target)))
                assert results[0] == results[1], (
                    operation, inputs, target, results)
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
        print('pkv: %d BITOP checks passed' % run(port))
        if args.redis:
            with server(os.path.abspath(args.redis), True) as reference:
                print('Redis: %d BITOP checks passed' % run(reference))
                print('pkv/Redis: %d BITOP comparisons passed' %
                      compare(port, reference))


if __name__ == '__main__':
    main()
