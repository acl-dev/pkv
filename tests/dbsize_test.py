#!/usr/bin/env python3
"""Stored key counts across types, logical DBs and staged transactions."""
import argparse
from integration_test import Client, ErrorReply, server


def run(port):
    client = Client(port)
    count = 0

    def check(expected, *args):
        nonlocal count
        got = client.call(*args)
        assert type(got) is type(expected) and got == expected, (
            args, got, expected)
        count += 1

    try:
        check(0, 'DBSIZE')
        check(b'OK', 'SET', b'', b'')
        check(b'OK', 'SET', b'a\0b', b'x' * 70000)
        check(1, 'HSET', 'hash', 'field', b'v' * 70000)
        check(1, 'LPUSH', 'list', b'v' * 70000)
        check(1, 'SADD', 'set', b'v' * 70000)
        check(1, 'ZADD', 'zset', 1, b'v' * 70000)
        check(6, 'DBSIZE')
        check(b'OK', 'SET', b'a\0b', 'replacement')
        check(6, 'DBSIZE')
        check(1, 'PEXPIREAT', b'a\0b', 4102444800123)
        check(6, 'DBSIZE')
        check(1, 'COPY', 'hash', 'copy')
        check(7, 'DBSIZE')
        check(b'OK', 'RENAME', 'copy', 'renamed')
        check(7, 'DBSIZE')
        check(1, 'DEL', 'renamed', 'renamed', 'missing')
        check(6, 'DBSIZE')
        check(b'OK', 'SELECT', 1)
        check(0, 'DBSIZE')
        check(b'OK', 'SET', 'separate', 'v')
        check(1, 'DBSIZE')
        check(b'OK', 'SELECT', 0)
        check(6, 'DBSIZE')
        check(1, 'RPUSH', 'single-delete', b'v' * 70000)
        check(7, 'DBSIZE')
        check(1, 'DEL', 'single-delete')
        check(0, 'DEL', 'single-delete')
        check(6, 'DBSIZE')
        check(b'OK', 'WATCH', 'hash')
        check(6, 'DBSIZE')
        check(b'OK', 'MULTI')
        check(b'QUEUED', 'SET', 'new', 'v')
        check(b'QUEUED', 'DBSIZE')
        check(b'QUEUED', 'DEL', 'new')
        check(b'QUEUED', 'DBSIZE')
        check(b'QUEUED', 'SET', 'new', 'again')
        check(b'QUEUED', 'DBSIZE')
        check([b'OK', 7, 1, 6, b'OK', 7], 'EXEC')
        check(7, 'DBSIZE')
        check(b'OK', 'MULTI')
        check(b'QUEUED', 'SET', 'discard', 'v')
        check(b'QUEUED', 'DBSIZE')
        check(b'OK', 'DISCARD')
        check(7, 'DBSIZE')
        assert isinstance(client.call('DBSIZE', 'extra'), ErrorReply)
        assert client.call('COMMAND', 'GETKEYS', 'DBSIZE') == ErrorReply(
            b'ERR The command has no key arguments')
    finally:
        client.close()
    return count


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--pkv', required=True)
    parser.add_argument('--redis', required=True)
    args = parser.parse_args()
    with server(args.redis, redis=True) as port:
        reference = run(port)
    with server(args.pkv) as port:
        checks = run(port)
    assert reference == checks
    with server(args.pkv, cluster=True) as port:
        client = Client(port)
        try:
            assert client.call('DBSIZE') == 0
            assert client.call('MULTI') == b'OK'
            assert client.call('DBSIZE') == b'QUEUED'
            assert client.call('EXEC') == [0]
        finally:
            client.close()
    print('DBSIZE assertions per server:', checks)


if __name__ == '__main__':
    main()
