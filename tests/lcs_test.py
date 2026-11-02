#!/usr/bin/env python3
"""LCS differential tests for complete RESP2 options and key routing."""
import argparse
import random
from integration_test import Client, ErrorReply, server


def exercise(port, reference):
    a, b = Client(port), Client(reference)
    checks = 0

    def compare(*args):
        nonlocal checks
        got, expected = a.call(*args), b.call(*args)
        assert type(got) is type(expected) and got == expected, (
            args, got, expected)
        checks += 1
        return got

    try:
        key1, key2 = b'{lcs}\0first', b'{lcs}\xffsecond'
        options = [(), ('LEN',), ('IDX',), ('IDX', 'WITHMATCHLEN'),
                   ('IDX', 'MINMATCHLEN', 2, 'WITHMATCHLEN'),
                   ('MINMATCHLEN', -1), ('WITHMATCHLEN',),
                   ('IDX', 'MINMATCHLEN', 999), ('LEN', 'LEN')]
        rng = random.Random(91724)
        samples = [(b'ohmytext', b'mynewtext'), (b'', b''),
                   (b'abc', b''), (b'abc', b'xyz'),
                   (b'abcabc', b'acbacb'), (b'\0\xff\r\n', b'\xff\0\r\n')]
        samples += [(bytes(rng.randrange(5) for _ in range(rng.randrange(30))),
                     bytes(rng.randrange(5) for _ in range(rng.randrange(30))))
                    for _ in range(100)]
        samples += [(b'x' * 70000, b'x\0x')]
        for left, right in samples:
            compare('MSET', key1, left, key2, right)
            for option in options:
                compare('LCS', key1, key2, *option)
        for option in [('LEN', 'IDX'), ('IDX', 'LEN'), ('MINMATCHLEN',),
                       ('MINMATCHLEN', '1x'), ('MINMATCHLEN', 2 ** 63),
                       ('unknown',),
                       ('MINMATCHLEN', 2, 'MINMATCHLEN', 1, 'IDX')]:
            compare('LCS', key1, key2, *option)
        assert a.call('LCS', key1, key2, b'LEN\0x') == \
            ErrorReply(b'ERR syntax error')
        compare('LCS', key1, key1, 'LEN')
        compare('DEL', key1, key2)
        compare('LCS', key1, key2)
        compare('LCS', key1, key2, 'IDX', 'WITHMATCHLEN')
        compare('HSET', key1, 'f', 'v')
        for option in [(), ('LEN', 'IDX'), ('bad',), ('MINMATCHLEN', 'x')]:
            compare('LCS', key1, key2, *option)
        compare('DEL', key1)
        compare('SET', key1, b'x' * 12000)
        compare('SET', key2, b'y' * 12000)
        for option in [(), ('LEN',), ('IDX',)]:
            compare('LCS', key1, key2, *option)
        compare('PEXPIREAT', key1, 1)
        compare('LCS', key1, key2, 'LEN')
        compare('MSET', key1, 'abc', key2, 'abc')
        compare('PEXPIREAT', key1, 4102444800123)
        compare('WATCH', key1, key2)
        compare('LCS', key1, key2, 'IDX')
        compare('PEXPIRETIME', key1)
        compare('MULTI')
        compare('SET', key2, 'axc')
        compare('LCS', key1, key2, 'IDX', 'WITHMATCHLEN')
        compare('EXEC')
        compare('SELECT', 1)
        compare('LCS', key1, key2)
        compare('SELECT', 0)
        compare('LCS', key1, key2, 'LEN')
        compare('SET', b'', b'\0xyz')
        compare('SET', key2, b'xy')
        compare('LCS', b'', key2, 'IDX', 'WITHMATCHLEN')
        for args in [('LCS',), ('LCS', key1)]:
            compare(*args)
    finally:
        a.close()
        b.close()
    return checks


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--pkv', required=True)
    parser.add_argument('--redis', required=True)
    args = parser.parse_args()
    with server(args.redis, redis=True) as ref, server(args.pkv) as port:
        checks = exercise(port, ref)
    with server(args.pkv, cluster=True) as port:
        c = Client(port)
        try:
            assert c.call('LCS', '{a}x', '{b}x') == ErrorReply(
                b'CROSSSLOT Keys in request don\'t hash to the same slot')
            assert c.call('COMMAND', 'GETKEYS', 'LCS', 'a', 'b', 'LEN') == \
                [b'a', b'b']
            assert c.call('MULTI') == b'OK'
            assert isinstance(c.call('LCS', '{a}x', '{b}x'), ErrorReply)
            assert isinstance(c.call('EXEC'), ErrorReply)
            slot = c.call('CLUSTER', 'KEYSLOT', '{same}a')
            assert c.call('CLUSTER', 'ADDSLOTS', slot) == b'OK'
            assert c.call('MSET', '{same}a', 'abc', '{same}b', 'axc') == b'OK'
            assert c.call('LCS', '{same}a', '{same}b', 'LEN') == 2
            assert c.call('MULTI') == b'OK'
            assert c.call('LCS', '{same}a', '{same}b') == b'QUEUED'
            assert c.call('EXEC') == [b'ac']
        finally:
            c.close()
    print('LCS differential assertions:', checks)


if __name__ == '__main__':
    main()
