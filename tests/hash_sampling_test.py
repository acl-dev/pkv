#!/usr/bin/env python3
"""HRANDFIELD framing, sampling, binary data and atomic snapshot checks."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import os
import threading

from integration_test import Client, ErrorReply, server


def run(port):
    checks = 0
    client = Client(port)

    def check(expected, *args):
        nonlocal checks
        actual = client.call(*args)
        assert type(actual) is type(expected) and actual == expected, (
            args, actual, expected)
        checks += 1

    def error(prefix, *args):
        nonlocal checks
        actual = client.call(*args)
        assert isinstance(actual, ErrorReply) and actual.startswith(prefix), (
            args, actual)
        checks += 1

    key = b'hash-random\0' + bytes(range(256))
    fields = {b'': b'', b'f\0x': b'v\0x', bytes(range(256)): b'\xff',
              b'large': b'v' * 70000}
    try:
        check(None, 'HRANDFIELD', key)
        for count in (0, 1, -1, 100, -100, 9223372036854775807):
            check([], 'HRANDFIELD', key, count)
        check([], 'HRANDFIELD', key, -100, 'WITHVALUES')
        check(4, 'HSET', key,
              *[part for pair in fields.items() for part in pair])
        check(1, 'PEXPIREAT', key, 4102444800123)
        for _ in range(50):
            assert client.call('HRANDFIELD', key) in fields
            checks += 1
        for count in (0, 1, 2, 3, 4, 5, 100, 9223372036854775807,
                      -1, -2, -20):
            for values in (False, True):
                if values and count > 4611686018427387903:
                    continue
                result = client.call('HRANDFIELD', key, count,
                                     *(('wItHvAlUeS',) if values else ()))
                assert type(result) is list
                names = result[::2] if values else result
                assert len(names) == (min(count, 4) if count >= 0 else -count)
                assert all(name in fields for name in names)
                if count >= 0:
                    assert len(set(names)) == len(names)
                if values:
                    assert result[1::2] == [fields[name] for name in names]
                checks += 1
        check(4102444800123, 'PEXPIRETIME', key)
        assert dict(zip(*(iter(client.call('HGETALL', key)),) * 2)) == fields
        checks += 1
        check(1, 'HSET', 'hash-random-single', b'', b'')
        check(b'', 'HRANDFIELD', 'hash-random-single')
        check([b''] * 20, 'HRANDFIELD', 'hash-random-single', -20)
        check([b'', b''] * 20, 'HRANDFIELD', 'hash-random-single', -20,
              'WITHVALUES')
        check(b'OK', 'SET', 'hash-random-wrong', 'value')
        for candidate in (key, 'hash-random-wrong', 'hash-random-absent'):
            for invalid in ('', '01', '+1', '-0', '1.5', '9223372036854775808',
                            b'1\0', 'WITHVALUES'):
                error(b'ERR value is not an integer or out of range',
                      'HRANDFIELD', candidate, invalid)
            error(b'ERR value is out of range, value must between',
                  'HRANDFIELD', candidate, -9223372036854775808)
            for invalid in ('WITHVALUE', 'OTHER'):
                error(b'ERR syntax error', 'HRANDFIELD', candidate, 1, invalid)
            error(b'ERR syntax error', 'HRANDFIELD', candidate, 1,
                  'WITHVALUES', 'WITHVALUES')
            for count in (4611686018427387904, -4611686018427387904):
                error(b'ERR value is out of range', 'HRANDFIELD', candidate,
                      count, 'WITHVALUES')
        error(b'ERR wrong number of arguments', 'HRANDFIELD')
        for count in (0, 1, -1):
            error(b'WRONGTYPE ', 'HRANDFIELD', 'hash-random-wrong', count)
        error(b'WRONGTYPE ', 'HRANDFIELD', 'hash-random-wrong')
        check([b'', b''], 'HRANDFIELD', 'hash-random-single', 1,
              b'WITHVALUES\0suffix')
        commands = [('HRANDFIELD', key, 1, 'WITHVALUE'),
                    ('HRANDFIELD', 'hash-random-single'), ('PING',)]
        client.socket.sendall(b''.join(client.encode(args)
                                      for args in commands))
        assert isinstance(client.read(), ErrorReply)
        assert client.read() == b'' and client.read() == b'PONG'
        checks += 3
        check(1, 'PEXPIREAT', key, 1)
        check(None, 'HRANDFIELD', key)
        check([], 'HRANDFIELD', key, -20, 'WITHVALUES')
        check(10, 'HSET', 'hash-random-race',
              *[part for i in range(10) for part in (str(i), 'initial')])
    finally:
        client.close()

    start = threading.Barrier(4)

    def worker(number):
        connection = Client(port)
        try:
            start.wait(timeout=10)
            for step in range(30):
                if number == 0:
                    epoch = '%d:%d' % (number, step)
                    assert connection.call('HSET', 'hash-random-race',
                        *[part for i in range(10) for part in
                          (str(i), epoch)]) == 0
                else:
                    result = connection.call('HRANDFIELD', 'hash-random-race',
                                             -20, 'WITHVALUES')
                    assert len(result) == 40 and len(set(result[1::2])) == 1
                    assert all(name in [str(i).encode() for i in range(10)]
                               for name in result[::2])
            return 30
        finally:
            connection.close()

    with ThreadPoolExecutor(max_workers=4) as pool:
        checks += sum(pool.map(worker, range(4)))
    return checks


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--pkv', required=True)
    parser.add_argument('--redis')
    args = parser.parse_args()
    for binary, reference in ((args.pkv, False), (args.redis, True)):
        if binary:
            with server(os.path.abspath(binary), reference) as port:
                print('%s: %d Hash sampling checks passed' % (
                    'Redis' if reference else 'pkv', run(port)))
                if not reference:
                    connection = Client(port)
                    try:
                        for count, flags in [(-9223372036854775807, ()),
                            (-4611686018427387903, ('WITHVALUES',))]:
                            actual = connection.call('HRANDFIELD',
                                'hash-random-single', count, *flags)
                            assert actual == ErrorReply(
                                b'ERR requested sample is too large')
                            assert connection.call('HGET',
                                'hash-random-single', b'') == b''
                            assert connection.call('PING') == b'PONG'
                    finally:
                        connection.close()


if __name__ == '__main__':
    main()
