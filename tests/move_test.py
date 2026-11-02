#!/usr/bin/env python3
"""Public MOVE semantics against an isolated reference Redis."""
import argparse
import contextlib
import os
from pathlib import Path
import tempfile
from concurrent.futures import ThreadPoolExecutor
import time
from integration_test import Client, ErrorReply, server
from ordered_live_test import commands, view


def compare(a, b, *args):
    actual, expected = a.call(*args), b.call(*args)
    assert actual == expected, (args, actual, expected)
    assert isinstance(actual, ErrorReply) == isinstance(expected, ErrorReply)
    return actual


def arguments(a, b):
    compare(a, b, 'SELECT', 0)
    compare(a, b, 'SET', 'move:selection', 'unchanged')
    for args in [(), ('key',), ('key', 7, 'extra')]:
        compare(a, b, 'MOVE', *args)
    for index in [0, -1, 16, 'x', '1.0', '+1', '01', b'1\0',
                  2147483648, -2147483649, 2**63, -(2**63) - 1, 7, 15]:
        compare(a, b, 'MOVE', 'absent', index)
        assert compare(a, b, 'GET', 'move:selection') == b'unchanged'
    assert compare(a, b, 'COMMAND', 'INFO', 'MOVE')[0][1] == 3
    assert compare(a, b, 'COMMAND', 'GETKEYS', 'MOVE', b'k\0', 7) == [b'k\0']
    compare(a, b, 'COMMAND', 'GETKEYSANDFLAGS', 'MOVE', b'k\0', 7)


def values(a, b):
    for kind in ('string', 'hash', 'list', 'set', 'zset'):
        key = b'' if kind == 'string' else kind.encode() + b'\0move'
        compare(a, b, 'SELECT', 0)
        create, _ = commands(kind, key)
        compare(a, b, *create)
        compare(a, b, 'PEXPIREAT', key, 4102444800123)
        assert compare(a, b, 'MOVE', key, 7) == 1
        assert compare(a, b, 'TYPE', key) == b'none'
        compare(a, b, 'SELECT', 7)
        assert view(a, kind, key) == view(b, kind, key), kind
        assert compare(a, b, 'PEXPIRETIME', key) == 4102444800123
        compare(a, b, 'MOVE', key, 7)
        compare(a, b, 'SELECT', 0)
        compare(a, b, 'SET', key, 'occupied')
        compare(a, b, 'SELECT', 7)
        assert compare(a, b, 'MOVE', key, 0) == 0
        assert view(a, kind, key) == view(b, kind, key), kind
        compare(a, b, 'SELECT', 0)
        compare(a, b, 'DEL', key)
        compare(a, b, 'SELECT', 7)
        assert compare(a, b, 'MOVE', key, 0) == 1
        assert compare(a, b, 'TYPE', key) == b'none'
        compare(a, b, 'SELECT', 0)
        assert view(a, kind, key) == view(b, kind, key)


def expiry(a, b):
    for key, target_expired in [('move:expired-source', False),
                                ('move:expired-target', True)]:
        compare(a, b, 'SELECT', 0)
        compare(a, b, 'SET', key, 'source')
        if target_expired:
            compare(a, b, 'SELECT', 7)
            compare(a, b, 'RPUSH', key, b'v' * 70000)
        compare(a, b, 'PEXPIRE', key, 10)
        time.sleep(.03)
        compare(a, b, 'SELECT', 0)
        assert compare(a, b, 'MOVE', key, 7) == int(target_expired)
        compare(a, b, 'SELECT', 7)
        compare(a, b, 'GET', key)
    compare(a, b, 'SELECT', 0)


def transaction(a, b):
    key = b'exec\0move'
    compare(a, b, 'SELECT', 0)
    for args in [('MULTI',), ('SET', key, '41'), ('MOVE', key, 7),
                 ('GET', key), ('SELECT', 7), ('INCR', key),
                 ('MOVE', key, 0), ('MOVE', key, -1), ('SELECT', 0),
                 ('GET', key), ('SET', 'move:wrong-int', 'bad'),
                 ('INCR', 'move:wrong-int'), ('GET', key)]:
        compare(a, b, *args)
    result = compare(a, b, 'EXEC')
    assert result[:7] == [b'OK', 1, None, b'OK', 42, 1,
                          ErrorReply(b'ERR DB index is out of range')]
    assert result[-1] == b'42'
    for args in [('MULTI',), ('SET', 'move:discarded', 'v'),
                 ('MOVE', key), ('EXEC',), ('GET', 'move:discarded')]:
        compare(a, b, *args)


def watches(a, b, port, reference):
    key = 'move:watched'
    compare(a, b, 'SELECT', 0)
    compare(a, b, 'SET', key, 'value')
    with contextlib.ExitStack() as stack:
        clients = [Client(p) for p in (port, reference, port, reference)]
        for c in clients:
            stack.callback(c.close)
        x, y, z, w = clients
        compare(z, w, 'SELECT', 7)
        compare(x, y, 'WATCH', key)
        compare(z, w, 'WATCH', key)
        assert compare(a, b, 'MOVE', key, 7) == 1
        for left, right in [(x, y), (z, w)]:
            compare(left, right, 'MULTI')
            compare(left, right, 'PING')
            assert compare(left, right, 'EXEC') is None
        compare(a, b, 'SET', key, 'occupied')
        compare(x, y, 'WATCH', key)
        compare(z, w, 'WATCH', key)
        compare(a, b, 'SELECT', 7)
        assert compare(a, b, 'MOVE', key, 0) == 0
        for left, right in [(x, y), (z, w)]:
            compare(left, right, 'MULTI')
            compare(left, right, 'PING')
            assert compare(left, right, 'EXEC') == [b'PONG']
    compare(a, b, 'SELECT', 0)


def acl(a, b, port, reference):
    for rule in ['%R~move:acl', '%W~move:acl', '~move:acl']:
        compare(a, b, 'ACL', 'SETUSER', 'move-user', 'reset', 'on', 'nopass',
                '+move', '+select', rule)
        x, y = Client(port), Client(reference)
        try:
            compare(x, y, 'AUTH', 'move-user', 'anything')
            compare(a, b, 'ACL', 'DRYRUN', 'move-user', 'MOVE', 'move:acl', 7)
            compare(a, b, 'SET', 'move:acl', 'value')
            compare(x, y, 'MOVE', 'move:acl', 7)
        finally:
            x.close()
            y.close()


def wakeup(binary):
    with server(binary) as port:
        writer, reader = Client(port), Client(port)
        try:
            assert reader.call('SELECT', 7) == b'OK'
            assert reader.call('CLIENT', 'SETNAME', 'move-blocked') == b'OK'
            with ThreadPoolExecutor(max_workers=1) as pool:
                waiting = pool.submit(reader.call, 'BLPOP', 'move:ready', 10)
                deadline = time.monotonic() + 2
                while True:
                    rows = writer.call('CLIENT', 'LIST').splitlines()
                    if any(b'name=move-blocked ' in r and
                           b'cmd=blpop ' in r and b'flags=b ' in r
                           for r in rows):
                        break
                    assert time.monotonic() < deadline, rows
                    time.sleep(.01)
                assert writer.call('RPUSH', 'move:ready', 'ready') == 1
                assert writer.call('MOVE', 'move:ready', 7) == 1
                assert waiting.result(timeout=12) == [b'move:ready', b'ready']
        finally:
            writer.close()
            reader.close()


def cluster(binary, redis):
    with server(binary, cluster=True) as port, \
            server(redis, redis=True, cluster=True) as reference:
        a, b = Client(port), Client(reference)
        try:
            for args in [('MOVE', 'key', 7), ('MOVE', 'key', 'x'),
                         ('MOVE', 'key', 0), ('MOVE', 'key')]:
                compare(a, b, *args)
            for first in range(0, 16384, 256):
                slots = list(range(first, first + 256))
                assert compare(a, b, 'CLUSTER', 'ADDSLOTS', *slots) == b'OK'
            deadline = time.monotonic() + 5
            while b'cluster_state:ok' not in b.call('CLUSTER', 'INFO'):
                assert time.monotonic() < deadline
                time.sleep(.05)
            for index in [0, 7, 'x', -1]:
                assert compare(a, b, 'MOVE', 'key', index) == ErrorReply(
                    b'ERR MOVE is not allowed in cluster mode')
        finally:
            a.close()
            b.close()


def persistent(binary):
    if os.environ.get('PKV_TEST_BACKEND') not in ('rdb', 'wdb'):
        return
    kinds = ('string', 'hash', 'list', 'set', 'zset')
    with tempfile.TemporaryDirectory(prefix='pkv-move-restart-') as directory:
        root = Path(directory) / 'data'
        expected = {}
        with server(binary, storage_path=root) as port:
            c = Client(port)
            try:
                for kind in kinds:
                    key = kind.encode() + b'\0persistent'
                    assert c.call(*commands(kind, key)[0]) is not None
                    expected[kind] = view(c, kind, key)
                    assert c.call('PEXPIREAT', key, 4102444800123) == 1
                    assert c.call('MOVE', key, 7) == 1
            finally:
                c.close()
        assert (root / '.pkv-move.domain').is_file()
        assert not (root / '.pkv-move.decision').exists()
        with server(binary, storage_path=root) as port:
            c = Client(port)
            try:
                for kind in kinds:
                    key = kind.encode() + b'\0persistent'
                    assert c.call('TYPE', key) == b'none'
                    assert c.call('SELECT', 7) == b'OK'
                    assert view(c, kind, key) == expected[kind]
                    assert c.call('PEXPIRETIME', key) == 4102444800123
                    assert c.call('MOVE', key, 0) == 1
                    assert c.call('SELECT', 0) == b'OK'
            finally:
                c.close()
        off = Path(directory) / 'disabled'
        with server(binary, storage_path=off, move_journal_path='off') as port:
            c = Client(port)
            try:
                assert c.call('SET', 'key', 'value') == b'OK'
                assert c.call('MOVE', 'key', 7) == ErrorReply(
                    b'ERR MOVE requires an enabled native journal')
                assert c.call('GET', 'key') == b'value'
                assert c.call('MOVE', 'missing', 7) == 0
            finally:
                c.close()
        assert not (off / '.pkv-move.domain').exists()


def run(binary, redis):
    with server(binary) as port, server(redis, redis=True) as reference:
        a, b = Client(port), Client(reference)
        try:
            arguments(a, b)
            values(a, b)
            expiry(a, b)
            transaction(a, b)
            watches(a, b, port, reference)
            acl(a, b, port, reference)
        finally:
            a.close()
            b.close()
    wakeup(binary)
    cluster(binary, redis)
    persistent(binary)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--pkv', required=True)
    parser.add_argument('--redis', required=True)
    args = parser.parse_args()
    run(args.pkv, args.redis)
    print('Public MOVE values/TTL/EXEC/WATCH/ACL/readiness passed')
