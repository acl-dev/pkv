#!/usr/bin/env python3
"""KEYS patterns, transactions and local cluster enumeration."""
import argparse
import random
from integration_test import Client, ErrorReply, server
from resp3_client import RespClient, Reply


def compare(port, reference):
    a, b = Client(port), Client(reference)
    count = 0

    def check(*args):
        nonlocal count
        got, expected = a.call(*args), b.call(*args)
        if args[0] == 'KEYS' and isinstance(got, list):
            assert len(got) == len(set(got)), args
            got, expected = set(got), set(expected)
        assert type(got) is type(expected) and got == expected, (
            args, got, expected)
        count += 1
        return got

    try:
        check('KEYS', '*')
        rng = random.Random(71841)
        names = [b'', b'a\0b', b'\xff\xfe', b'a*b', b'a?b', b'a[b',
                 b'\r\n', b'\\', b'ordinary']
        names += [bytes(rng.randrange(256) for _ in range(rng.randrange(1, 20)))
                  for _ in range(160)]
        for index, name in enumerate(names):
            check('SET', name, 'value-' + str(index))
        check('SET', 'large', b'x' * 70000)
        check('HSET', 'hash', 'field', 'value')
        check('LPUSH', 'list', 'one', 'two')
        check('SADD', 'set', 'one', 'two')
        check('ZADD', 'zset', 1, 'one', 2, 'two')
        check('PEXPIREAT', 'large', 4102444800123)
        patterns = [b'*', b'', b'?', b'a*', b'a?b', b'a\\*b', b'[a-z]*',
                    b'[^a]*', b'a\0*', b'\xff*', b'[', b'\\', b'*\\',
                    b'[\xff-\x01]*', b'*\0*', b'absent']
        patterns += [bytes(rng.choice(b'ab?*[]^\\')
                           for _ in range(rng.randrange(9)))
                     for _ in range(120)]
        for pattern in patterns:
            check('KEYS', pattern)
        check('SET', 'expired', 'gone')
        check('PEXPIREAT', 'expired', 1)
        assert b'expired' not in check('KEYS', '*')
        check('PEXPIRETIME', 'large')
        check('WATCH', 'large')
        check('KEYS', 'large')
        check('MULTI')
        check('PING')
        check('EXEC')
        check('SELECT', 1)
        check('KEYS', '*')
        check('SET', 'other-db', 'v')
        check('KEYS', '*')
        check('SELECT', 0)
        check('SET', 'tx:old', 'before')
        check('SET', 'tx:overwritten', 'before')
        check('MULTI')
        for args in [('DEL', 'tx:old'), ('SET', 'tx:new', 'after'),
                     ('SET', 'tx:overwritten', 'after'), ('KEYS', 'tx:*')]:
            check(*args)
        got, expected = a.call('EXEC'), b.call('EXEC')
        assert got[:3] == expected[:3]
        assert len(got[3]) == len(set(got[3]))
        assert set(got[3]) == set(expected[3]) == {
            b'tx:new', b'tx:overwritten'}
        check('KEYS', 'tx:*')
        check('KEYS')
        check('KEYS', '*', 'extra')
        assert a.call('COMMAND', 'GETKEYS', 'KEYS', '*') == ErrorReply(
            b'ERR The command has no key arguments')
    finally:
        a.close()
        b.close()
    return count


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--pkv', required=True)
    parser.add_argument('--redis', required=True)
    args = parser.parse_args()
    with server(args.redis, redis=True) as ref, server(args.pkv) as port:
        count = compare(port, ref)
    with server(args.pkv, cluster=True) as port:
        client = Client(port)
        resp3 = RespClient(port)
        try:
            assert client.call('KEYS', '*') == []
            key = '{local}key'
            slot = client.call('CLUSTER', 'KEYSLOT', key)
            assert client.call('CLUSTER', 'ADDSLOTS', slot) == b'OK'
            assert client.call('SET', key, 'value') == b'OK'
            assert client.call('KEYS', '*') == [key.encode()]
            assert client.call('MULTI') == b'OK'
            assert client.call('KEYS', '*') == b'QUEUED'
            assert client.call('EXEC') == [[key.encode()]]
            assert resp3.call('HELLO', 3).kind == b'%'
            reply = Reply(b'*', [Reply(b'$', key.encode())])
            assert resp3.call('KEYS', '*') == reply
            assert resp3.call('MULTI').value == b'OK'
            assert resp3.call('KEYS', '*').value == b'QUEUED'
            assert resp3.call('EXEC') == Reply(b'*', [reply])
        finally:
            client.close()
            resp3.close()
    print('KEYS differential assertions:', count)


if __name__ == '__main__':
    main()
