#!/usr/bin/env python3
"""Legacy GEO radius commands, optional targets and bounded-store semantics."""
import argparse
import contextlib
import os
import random
import tempfile
import time
from integration_test import Client, ErrorReply, server
from resp3_client import RespClient, canonical


def compare(first, second):
    checked = 0

    def call(*args):
        nonlocal checked
        a, b = first.call(*args), second.call(*args)
        assert type(a) is type(b) and a == b, (args, a, b)
        checked += 1
        return a

    call('GEOADD', 'src', 13.361389, 38.115556, 'a',
         15.087269, 37.502669, 'b')
    for member in [False, True]:
        root = 'GEORADIUSBYMEMBER' if member else 'GEORADIUS'
        origin = ['a', 200, 'km'] if member else [15, 37, 200, 'km']
        for name in [root, root + '_RO']:
            for options in [[], ['ASC'], ['DESC'], ['COUNT', 1],
                            ['COUNT', 1, 'ANY'], ['ANY', 'COUNT', 2, 'DESC'],
                            ['WITHDIST', 'WITHHASH', 'WITHCOORD', 'ASC']]:
                call(name, 'src', *origin, *options)
        for tail in [['STORE', 'out'], ['STOREDIST', 'out'],
                     ['STORE', 'ignored', 'STOREDIST', 'out'],
                     ['STOREDIST', 'ignored', 'STORE', 'out'],
                     ['STORE', 'one', 'STORE', 'out', 'STOREDIST', 'dist'],
                     [b'STORE\0suffix', b'binary\0\xff']]:
            call(root, 'src', *origin, *tail)
            for key in ['out', 'dist', 'ignored', 'one', b'binary\0\xff']:
                call('ZRANGE', key, 0, -1, 'WITHSCORES')
    randomizer = random.Random(7419)
    points = []
    for i in range(400):
        points += [randomizer.uniform(-180, 180),
                   randomizer.uniform(-85, 85), str(i)]
    call('GEOADD', 'global', *points)
    for i in range(30):
        member = i % 2
        root = 'GEORADIUSBYMEMBER' if member else 'GEORADIUS'
        origin = [str(randomizer.randrange(400)),
                  randomizer.uniform(0, 25000), 'km'] if member else [
                  randomizer.uniform(-180, 180), randomizer.uniform(-85, 85),
                  randomizer.uniform(0, 25000), 'km']
        for name in [root, root + '_RO']:
            for options in [[], ['ASC', 'COUNT', 3], ['DESC', 'COUNT', 7],
                            ['COUNT', 2, 'ANY'],
                            ['WITHDIST', 'WITHHASH', 'WITHCOORD', 'ASC']]:
                call(name, 'global', *origin, *options)
        call(root, 'global', *origin, 'COUNT', 2, 'STOREDIST', 'random-result')
        call('ZRANGE', 'random-result', 0, -1, 'WITHSCORES')
    call('SET', 'wrong', 'string')
    for key in ['src', 'missing', 'wrong']:
        for root in ['GEORADIUS', 'GEORADIUS_RO', 'GEORADIUSBYMEMBER',
                     'GEORADIUSBYMEMBER_RO']:
            origin = ['a', 'bad', 'bad'] if 'BYMEMBER' in root else [
                      'bad', 0, 'bad', 'bad']
            for tail in [[], ['COUNT', 0], ['ANY'], ['STORE', 'out'],
                         ['WITHDIST', 'STORE', 'out']]:
                call(root, key, *origin, *tail)
    for root in ['GEORADIUSBYMEMBER', 'GEORADIUSBYMEMBER_RO']:
        call(root, 'missing', 'a', 'bad-radius', 'bad-unit')
        call(root, 'src', 'missing', 'bad-radius', 'bad-unit')
    for longitude, latitude in [('inf', 0), (181, 0), (0, 86)]:
        call('GEORADIUS', 'src', longitude, latitude, 1, 'm')
    for tail in [['COUNT', '01'], ['COUNT', -1], ['COUNT', 2**63],
                 ['ANY'], ['BYBOX', 1, 2, 'm'], ['FROMMEMBER', 'a'],
                 ['STORE'], ['STOREDIST'], ['WITHDIST', 'STORE', 'out']]:
        call('GEORADIUS', 'src', 0, 0, 1, 'm', *tail)
    call('PEXPIREAT', 'src', 4102444800000)
    call('SET', 'replaced', 'old')
    call('GEORADIUSBYMEMBER', 'src', 'a', 0, 'm', 'STOREDIST', 'replaced')
    call('PEXPIRETIME', 'src')
    call('PEXPIRETIME', 'replaced')
    assert call('ZSCORE', 'replaced', 'a') == b'0'
    call('GEORADIUSBYMEMBER', 'src', 'a', 0, 'm', 'STORE', 'src')
    call('ZRANGE', 'src', 0, -1, 'WITHSCORES')
    call('PEXPIRETIME', 'src')
    call('SET', 'empty-result', 'old')
    call('GEORADIUSBYMEMBER', 'missing', 'a', 'bad', 'bad',
         'STORE', 'empty-result')
    call('EXISTS', 'empty-result')
    large = b'x' * 40000
    call('GEOADD', 'large', 1, 2, large)
    call('GEORADIUSBYMEMBER_RO', 'large', large, 0, 'm', 'WITHHASH')
    call('GEORADIUSBYMEMBER', 'large', large, 0, 'm', 'STORE', 'large-result')
    call('ZRANGE', 'large-result', 0, -1, 'WITHSCORES')
    targets = [('GEORADIUS', 'src', 0, 0, 10, 'km', 'STORE', 'one',
                'STOREDIST', 'two', 'STORE', 'three'),
               ('GEORADIUSBYMEMBER', 'src', 'm', 10, 'km',
                'COUNT', 'STORE', 'x'),
               ('GEORADIUS', 'src', 0, 0, 10, 'STORE', 'unit-target'),
               ('GEORADIUS_RO', 'src', 0, 0, 10, 'km', 'STORE', 'ignored'),
               ('GEORADIUS', 'src', 0, 0, 10, 'km', 'STORE',
                'STOREDIST', 'literal'),
               ('GEORADIUS', 'src', 0, 0, 10, 'km', 'STORE')]
    for args in targets:
        for operation in ['GETKEYS', 'GETKEYSANDFLAGS']:
            call('COMMAND', operation, *args)
    for name in ['GEORADIUS', 'GEORADIUS_RO', 'GEORADIUSBYMEMBER',
                 'GEORADIUSBYMEMBER_RO']:
        call('COMMAND', 'INFO', name)
    call('SELECT', 3)
    call('GEOADD', 'queued', 1, 2, 'a')
    call('MULTI')
    call('GEORADIUSBYMEMBER', 'queued', 'a', 0, 'm', 'STORE', 'result')
    call('GEORADIUSBYMEMBER_RO', 'result', 'a', 0, 'm', 'WITHDIST')
    call('EXEC')
    call('SELECT', 0)
    return checked


def typed(ports):
    clients = [RespClient(port) for port in ports]
    checks = 0
    try:
        for protocol in [3, 2]:
            for client in clients:
                client.call('HELLO', protocol)
            for args in [('GEORADIUS_RO', 'global', 0, 0, 25000, 'km',
                          'COUNT', 2, 'WITHDIST', 'WITHHASH', 'WITHCOORD'),
                         ('GEORADIUSBYMEMBER_RO', 'global', '0', 0, 'm'),
                         ('GEORADIUSBYMEMBER', 'global', '0', 0, 'm',
                          'STOREDIST', 'typed'),
                         ('GEORADIUSBYMEMBER_RO', 'missing', 'x',
                          'invalid', 'invalid')]:
                a, b = [client.call(*args) for client in clients]
                assert canonical(a) == canonical(b), (args, a, b)
                checks += 1
    finally:
        for client in clients:
            client.close()
    return checks


def permissions(port, hardened=False):
    admin, user = Client(port), Client(port)
    try:
        assert admin.call('GEOADD', 'src:geo', 1, 2, 'a') == 1
        assert admin.call('ACL', 'SETUSER', 'radius', 'on', 'nopass',
                          '+@geo', '%R~src:*', '%W~out:*') == b'OK'
        assert user.call('AUTH', 'radius', 'unused') == b'OK'
        assert user.call('GEORADIUSBYMEMBER_RO', 'src:geo', 'a', 0, 'm') == (
            [b'a'])
        assert user.call('GEORADIUSBYMEMBER', 'src:geo', 'a', 0, 'm',
                         'STORE', 'out:geo') == 1
        assert isinstance(user.call('GEORADIUSBYMEMBER', 'src:geo', 'a', 0,
                                    'm', 'STORE', 'forbidden'), ErrorReply)
        if hardened:
            assert admin.call('SET', 'forbidden', 'preserved') == b'OK'
            command = ('GEORADIUSBYMEMBER', 'src:geo', 'a', 0, 'm',
                       'STORE', 'out:first', 'STORE', 'forbidden')
            result = admin.call('ACL', 'DRYRUN', 'radius', *command)
            assert isinstance(result, bytes) and b'forbidden' in result and (
                result != b'OK'), result
            denied = user.call(*command)
            assert isinstance(denied, ErrorReply) and denied.startswith(
                b'NOPERM'), denied
            assert admin.call('GET', 'forbidden') == b'preserved'
            assert admin.call('EXISTS', 'out:first') == 0
    finally:
        admin.close()
        user.close()


def watch(port):
    first, second = Client(port), Client(port)
    try:
        assert first.call('GEOADD', 'watched', 1, 2, 'a') == 1
        args = ('watched', 'a', 0, 'm', 'STORE', 'dest')
        for _ in range(2):
            assert first.call('WATCH', 'dest') == b'OK'
            assert second.call('GEORADIUSBYMEMBER', *args) == 1
            assert first.call('MULTI') == b'OK'
            assert first.call('PING') == b'QUEUED'
            assert first.call('EXEC') is None
        assert first.call('PEXPIRE', 'watched', 1) == 1
        time.sleep(.01)
        assert first.call('GEORADIUSBYMEMBER', *args) == 0
        assert first.call('EXISTS', 'dest') == 0
    finally:
        first.close()
        second.close()


def cluster(binary, reference):
    with server(binary, redis=reference, cluster=True) as port:
        client = Client(port)
        try:
            for name, origin in [('GEORADIUS', [0, 0, 1, 'm']),
                                 ('GEORADIUSBYMEMBER', ['a', 1, 'm'])]:
                result = client.call(name, '{a}src', *origin, 'STORE', '{b}out')
                assert isinstance(result, ErrorReply) and result.startswith(
                    b'CROSSSLOT'), result
                # Reference preprocessing still checks metadata targets.
                result = client.call(name, '{a}src', *origin,
                                     'STORE', '{b}ignored', 'STORE', '{a}out')
                assert isinstance(result, ErrorReply) and result.startswith(
                    b'CROSSSLOT'), result
                assert client.call('MULTI') == b'OK'
                result = client.call(name, '{a}src', *origin,
                                     'STORE', '{b}ignored', 'STORE', '{a}out')
                assert isinstance(result, ErrorReply) and result.startswith(
                    b'CROSSSLOT'), result
                assert isinstance(client.call('EXEC'), ErrorReply)
                if name == 'GEORADIUS':
                    result = client.call(name, '{a}src', 0, 0, 1,
                                         'STORE', '{b}not-a-key')
                    assert isinstance(result, ErrorReply) and (
                        result.startswith(b'CLUSTERDOWN')), result
                assert client.call('PING') == b'PONG'
                if not reference:
                    result = client.call(name, '{a}src', *origin,
                                         'STORE', '{a}first',
                                         'STORE', '{b}last')
                    assert isinstance(result, ErrorReply) and (
                        result.startswith(b'CROSSSLOT')), result
        finally:
            client.close()


def persistence(binary):
    if os.environ.get('PKV_TEST_BACKEND', 'mdb') not in ('rdb', 'wdb'):
        return
    with tempfile.TemporaryDirectory(prefix='pkv-radius-restart-') as path:
        expected = None
        for turn in range(2):
            with server(binary, storage_path=path) as port:
                client = Client(port)
                try:
                    assert client.call('SELECT', 3) == b'OK'
                    if not turn:
                        client.call('GEOADD', 'src', 1, 2, 'a')
                        assert client.call('GEORADIUSBYMEMBER', 'src', 'a', 0,
                                           'm', 'STOREDIST', 'found') == 1
                        expected = client.call('ZRANGE', 'found', 0, -1,
                                               'WITHSCORES')
                    else:
                        assert client.call('ZRANGE', 'found', 0, -1,
                                           'WITHSCORES') == expected
                finally:
                    client.close()


def replication(binary):
    with server(binary) as source:
        with server(binary, replica_rpc_source='127.0.0.1:%d' % (
                source + 10000)) as replica:
            first, second = Client(source), Client(replica)
            try:
                deadline = time.monotonic() + 8
                while True:
                    first.call('SET', 'ready', 'yes')
                    if second.call('GET', 'ready') == b'yes':
                        break
                    assert time.monotonic() < deadline
                    time.sleep(.02)
                assert first.call('MULTI') == b'OK'
                assert first.call('GEOADD', 'geo', 1, 2, 'a', 3, 4, 'b') == (
                    b'QUEUED')
                assert first.call('GEORADIUSBYMEMBER', 'geo', 'a', 1000, 'km',
                                  'COUNT', 1, 'STORE', 'ignored', 'STOREDIST',
                                  'found') == b'QUEUED'
                assert first.call('EXEC') == [2, 1]
                for key in ['geo', 'found']:
                    expected = first.call('ZRANGE', key, 0, -1, 'WITHSCORES')
                    deadline = time.monotonic() + 8
                    while second.call('ZRANGE', key, 0, -1, 'WITHSCORES') != (
                            expected):
                        assert time.monotonic() < deadline
                        time.sleep(.02)
                assert second.call('EXISTS', 'ignored') == 0
                assert second.call('GEORADIUSBYMEMBER_RO', 'geo', 'a', 0,
                                   'm') == [b'a']
                result = second.call('GEORADIUSBYMEMBER', 'geo', 'a', 0, 'm')
                assert isinstance(result, ErrorReply) and result.startswith(
                    b'READONLY'), result
            finally:
                first.close()
                second.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--pkv', required=True)
    parser.add_argument('--redis', required=True)
    args = parser.parse_args()
    with contextlib.ExitStack() as stack:
        ports = [stack.enter_context(server(binary, redis=reference,
                 active_expire=False)) for binary, reference in
                 [(args.pkv, False), (args.redis, True)]]
        clients = [Client(port) for port in ports]
        try:
            expected = {b'geoadd', b'geopos', b'geohash', b'geodist',
                        b'geosearch', b'geosearchstore', b'georadius',
                        b'georadius_ro', b'georadiusbymember',
                        b'georadiusbymember_ro'}
            for client in clients:
                assert set(client.call('ACL', 'CAT', 'geo')) == expected
            checks = compare(*clients)
        finally:
            for client in clients:
                client.close()
        typed_checks = typed(ports)
        for i, port in enumerate(ports):
            permissions(port, hardened=i == 0)
            watch(port)
    cluster(args.pkv, False)
    cluster(args.redis, True)
    persistence(args.pkv)
    replication(args.pkv)
    print('GEO radius stable differential assertions:', checks)
    print('GEO radius typed differential assertions:', typed_checks)
    print('Radius options, atomic stores, ACL targets and cluster routing '
          'passed')


if __name__ == '__main__':
    main()
