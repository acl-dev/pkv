#!/usr/bin/env python3
"""Indexed GEO search, top-k, atomic stores and protocol contracts."""
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

    call('GEOADD', 'sicily', 13.361389, 38.115556, 'a',
         15.087269, 37.502669, 'b', 13.583333, 37.316667, 'c')
    for origin in [('FROMMEMBER', 'a'), ('FROMLONLAT', 15, 37)]:
        for shape in [('BYRADIUS', 200, 'km'), ('BYBOX', 400, 400, 'km')]:
            for options in [(), ('ASC',), ('DESC',), ('COUNT', 1),
                            ('COUNT', 1, 'ANY'), ('ANY', 'COUNT', 2, 'DESC'),
                            ('WITHDIST', 'WITHHASH', 'WITHCOORD', 'ASC')]:
                call('GEOSEARCH', 'sicily', *origin, *shape, *options)
    for shape in [('BYBOX', 0, 0, 'm'), ('BYBOX', 100, 0, 'm'),
                  ('BYBOX', 0, 100, 'm'), ('BYRADIUS', 0, 'm'),
                  ('BYRADIUS', 'inf', 'm'), ('BYBOX', 'inf', 'inf', 'm')]:
        call('GEOSEARCH', 'sicily', 'FROMMEMBER', 'a', *shape, 'WITHDIST')
    call('GEOSEARCHSTORE', 'self-distance', 'sicily', 'FROMMEMBER', 'a',
         'BYRADIUS', 0, 'm', 'STOREDIST')
    assert call('ZSCORE', 'self-distance', 'a') == b'0'
    randomizer = random.Random(787)
    points = []
    for i in range(600):
        points += [randomizer.uniform(-180, 180),
                   randomizer.uniform(-85, 85), str(i)]
    call('GEOADD', 'global', *points)
    for i in range(40):
        origin = ['FROMLONLAT', randomizer.uniform(-180, 180),
                  randomizer.uniform(-85, 85)] if i % 2 else [
                  'FROMMEMBER', str(randomizer.randrange(600))]
        shape = ['BYRADIUS', randomizer.uniform(0, 24000), 'km'] if i % 3 else [
                 'BYBOX', randomizer.uniform(0, 40000),
                 randomizer.uniform(0, 40000), 'km']
        for options in [[], ['ASC', 'COUNT', 7], ['DESC', 'COUNT', 9],
                        ['COUNT', 3, 'ANY'],
                        ['WITHDIST', 'WITHHASH', 'WITHCOORD', 'ASC']]:
            call('GEOSEARCH', 'global', *origin, *shape, *options)
        call('GEOSEARCHSTORE', 'result', 'global', *origin, *shape,
             'COUNT', 4, 'STOREDIST')
        call('ZRANGE', 'result', 0, -1, 'WITHSCORES')
    edge = [179.999, 0, 'east', -179.999, 0, 'west', 0, 84.9, 'north',
            0, -84.9, 'south', 180, 85.05112878, 'upper',
            -180, -85.05112878, 'lower']
    call('GEOADD', 'edge', *edge)
    for longitude, latitude in [(180, 0), (-180, 0), (0, 85), (0, -85),
                                (180, 85.05112878), (-180, -85.05112878)]:
        for radius in [1, 1000, 25000]:
            call('GEOSEARCH', 'edge', 'FROMLONLAT', longitude, latitude,
                 'BYRADIUS', radius, 'km', 'ASC', 'WITHHASH')
    call('PEXPIREAT', 'sicily', 4102444800000)
    call('SET', 'result', 'overwritten')
    call('GEOSEARCHSTORE', 'result', 'sicily', 'FROMMEMBER', 'a',
         'BYRADIUS', 200, 'km', 'COUNT', 2)
    call('PEXPIRETIME', 'sicily')
    call('TYPE', 'result')
    call('PEXPIRETIME', 'result')
    call('GEOSEARCHSTORE', 'sicily', 'sicily', 'FROMMEMBER', 'a',
         'BYRADIUS', 200, 'km', 'COUNT', 2, 'STOREDIST')
    call('ZRANGE', 'sicily', 0, -1, 'WITHSCORES')
    call('PEXPIRETIME', 'sicily')
    call('SET', 'deleted', 'old')
    call('GEOSEARCHSTORE', 'deleted', 'absent', 'FROMMEMBER', 'absent',
         'BYRADIUS', 1, 'm')
    call('EXISTS', 'deleted')
    call('GEOSEARCH', 'absent', 'FROMMEMBER', 'absent', 'BYRADIUS', 1, 'm')
    call('SET', 'wrong', 'string')
    cases = [(), ('FROMMEMBER', '0'), ('BYRADIUS', 1, 'm'),
             ('FROMMEMBER', '0', 'FROMLONLAT', 1, 2, 'BYRADIUS', 1, 'm'),
             ('FROMLONLAT', 1, 2, 'FROMMEMBER', '0', 'BYRADIUS', 1, 'm'),
             ('FROMMEMBER', '0', 'BYRADIUS', 1, 'm', 'BYBOX', 1, 1, 'm'),
             ('FROMMEMBER', '0', 'BYRADIUS', -1, 'm'),
             ('FROMMEMBER', '0', 'BYRADIUS', 'NaN', 'm'),
             ('FROMMEMBER', '0', 'BYBOX', 'bad', 'bad', 'm'),
             ('FROMMEMBER', '0', 'BYBOX', -1, 'bad', 'm'),
             ('FROMMEMBER', '0', 'BYBOX', -1, 1, 'm'),
             ('FROMMEMBER', '0', 'BYRADIUS', 1, 'bad-unit'),
             ('FROMMEMBER', '0', 'BYRADIUS', 1, 'm', 'ANY'),
             ('FROMMEMBER', '0', 'BYRADIUS', 1, 'm', 'COUNT', 0),
             ('FROMMEMBER', '0', 'BYRADIUS', 1, 'm', 'COUNT', '01'),
             ('FROMMEMBER', '0', 'BYRADIUS', 1, 'm', 'COUNT', 2**63),
             ('FROMMEMBER', 'missing', 'BYRADIUS', 'bad', 'bad'),
             ('FROMLONLAT', 181, 0, 'BYRADIUS', 1, 'm'),
             ('FROMLONLAT', 'bad', 0, 'BYRADIUS', 1, 'm'),
             ('FROMMEMBER', '0', 'BYRADIUS', 1, 'm', 'STOREDIST')]
    for key in ['global', 'wrong', 'absent']:
        for options in cases:
            call('GEOSEARCH', key, *options)
    for options in [('WITHDIST',), ('WITHHASH',), ('WITHCOORD',),
                    ('FROMMEMBER', '0', 'WITHDIST', 'BYRADIUS', 1, 'm')]:
        call('GEOSEARCHSTORE', 'result', 'global', *options)
    call('GEOSEARCH', 'global', 'FROMMEMBER', '0', 'FROMMEMBER', '1',
         'BYRADIUS', 1, 'km', 'BYRADIUS', 2, 'km', 'COUNT', 1, 'COUNT', 2)
    call('GEOSEARCH', 'global', b'FROMLONLAT\0suffix', 0, 0,
         'BYRADIUS', 25000, b'km\0suffix', 'COUNT', 2)
    large = b'x' * 40000
    call('GEOADD', 'large', 1, 2, large)
    call('GEOSEARCH', 'large', 'FROMMEMBER', large, 'BYRADIUS', 0, 'm',
         'WITHDIST', 'WITHHASH', 'WITHCOORD')
    call('GEOSEARCHSTORE', 'large-result', 'large', 'FROMMEMBER', large,
         'BYRADIUS', 0, 'm', 'STOREDIST')
    call('ZRANGE', 'large-result', 0, -1, 'WITHSCORES')
    call('SELECT', 3)
    call('GEOADD', 'queued', 1, 2, 'a', 3, 4, 'b')
    call('MULTI')
    call('GEOSEARCHSTORE', 'found', 'queued', 'FROMMEMBER', 'a',
         'BYRADIUS', 500, 'km', 'STOREDIST')
    call('ZRANGE', 'found', 0, -1, 'WITHSCORES')
    call('GEOSEARCH', 'queued', 'FROMMEMBER', 'a', 'BYBOX', 0, 0, 'm')
    call('EXEC')
    call('SELECT', 0)
    for args in [('GEOSEARCH', 'global', 'FROMMEMBER', '0',
                  'BYRADIUS', 1, 'm'),
                 ('GEOSEARCHSTORE', 'result', 'global', 'FROMMEMBER', '0',
                  'BYRADIUS', 1, 'm')]:
        call('COMMAND', 'GETKEYSANDFLAGS', *args)
    for command in ['GEOSEARCH', 'GEOSEARCHSTORE']:
        call('COMMAND', 'INFO', command)
    call('gEoSeArCh', 'global', 'BYRADIUS', 1, 'm', 'WITHDIST', 'COUNT', 1)
    return checked


def typed(ports):
    clients = [RespClient(port) for port in ports]
    checks = 0
    try:
        for protocol in [3, 2]:
            for client in clients:
                client.call('HELLO', protocol)
            for args in [('GEOSEARCH', 'global', 'FROMMEMBER', '0',
                          'BYRADIUS', 25000, 'km', 'COUNT', 2, 'WITHDIST',
                          'WITHHASH', 'WITHCOORD'),
                         ('GEOSEARCH', 'absent', 'FROMMEMBER', 'missing',
                          'BYRADIUS', 1, 'm'),
                         ('GEOSEARCHSTORE', 'typed-result', 'global',
                          'FROMMEMBER', '0', 'BYRADIUS', 25000, 'km',
                          'COUNT', 2),
                         ('GEOSEARCHSTORE', 'typed-result', 'global',
                          'FROMMEMBER', '0', 'BYBOX', 0, 0, 'm', 'STOREDIST')]:
                a, b = [client.call(*args) for client in clients]
                assert canonical(a) == canonical(b), (args, a, b)
                checks += 1
    finally:
        for client in clients:
            client.close()
    return checks


def watch_and_acl(port):
    first, second = Client(port), Client(port)
    search = ('FROMMEMBER', 'a', 'BYRADIUS', 0, 'm')
    try:
        assert first.call('GEOADD', 'src:geo', 1, 2, 'a') == 1
        assert first.call('WATCH', 'src:geo') == b'OK'
        assert second.call('GEOSEARCH', 'src:geo', *search) == [b'a']
        assert first.call('MULTI') == b'OK'
        assert first.call('PING') == b'QUEUED'
        assert first.call('EXEC') == [b'PONG']
        for _ in range(2):
            assert first.call('WATCH', 'dst:geo') == b'OK'
            assert second.call('GEOSEARCHSTORE', 'dst:geo', 'src:geo',
                               *search) == 1
            assert first.call('MULTI') == b'OK'
            assert first.call('PING') == b'QUEUED'
            assert first.call('EXEC') is None
        assert first.call('ACL', 'SETUSER', 'searcher', 'on', 'nopass',
                          '+geosearch', '+geosearchstore', '%R~src:*',
                          '%W~dst:*') == b'OK'
        assert second.call('AUTH', 'searcher', 'unused') == b'OK'
        assert second.call('GEOSEARCH', 'src:geo', *search) == [b'a']
        assert second.call('GEOSEARCHSTORE', 'dst:geo', 'src:geo', *search) == 1
        assert isinstance(second.call('GEOSEARCH', 'dst:geo', *search),
                          ErrorReply)
        assert isinstance(second.call('GEOSEARCHSTORE', 'bad', 'src:geo',
                                      *search), ErrorReply)
        assert first.call('PEXPIRE', 'src:geo', 1) == 1
        time.sleep(.01)
        assert second.call('GEOSEARCHSTORE', 'dst:geo', 'src:geo', *search) == 0
        assert first.call('EXISTS', 'dst:geo') == 0
    finally:
        first.close()
        second.close()


def persistence(binary):
    if os.environ.get('PKV_TEST_BACKEND', 'mdb') not in ('rdb', 'wdb'):
        return
    with tempfile.TemporaryDirectory(prefix='pkv-geo-search-restart-') as path:
        expected = None
        for turn in range(2):
            with server(binary, storage_path=path) as port:
                client = Client(port)
                try:
                    assert client.call('SELECT', 3) == b'OK'
                    if not turn:
                        assert client.call('GEOADD', 'geo', 1, 2, 'a') == 1
                        assert client.call('GEOSEARCHSTORE', 'found', 'geo',
                                           'FROMMEMBER', 'a', 'BYRADIUS', 0,
                                           'm', 'STOREDIST') == 1
                        expected = client.call('ZRANGE', 'found', 0, -1,
                                               'WITHSCORES')
                    else:
                        assert client.call('ZRANGE', 'found', 0, -1,
                                           'WITHSCORES') == expected
                        assert client.call('GEOSEARCH', 'geo', 'FROMMEMBER',
                                           'a', 'BYBOX', 0, 0, 'm') == [b'a']
                finally:
                    client.close()


def cluster(binary, reference):
    with server(binary, redis=reference, cluster=True) as port:
        client = Client(port)
        try:
            result = client.call('GEOSEARCHSTORE', '{a}out', '{b}geo',
                                 'FROMMEMBER', 'a', 'BYRADIUS', 1, 'm')
            assert isinstance(result, ErrorReply) and result.startswith(
                b'CROSSSLOT'), result
            for args in [('GEOSEARCH', '{a}geo', 'FROMMEMBER', '{b}member',
                          'BYRADIUS', -1, 'bad'),
                         ('GEOSEARCHSTORE', '{a}out', '{a}geo',
                          'FROMMEMBER', 'a', 'BYRADIUS', -1, 'bad')]:
                result = client.call(*args)
                assert isinstance(result, ErrorReply) and result.startswith(
                    b'CLUSTERDOWN'), result
                assert client.call('PING') == b'PONG'
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
                assert first.call('GEOSEARCHSTORE', 'found', 'geo',
                                  'FROMMEMBER', 'a', 'BYRADIUS', 1000, 'km',
                                  'COUNT', 1, 'STOREDIST') == b'QUEUED'
                assert first.call('EXEC') == [2, 1]
                for key in ['geo', 'found']:
                    expected = first.call('ZRANGE', key, 0, -1, 'WITHSCORES')
                    deadline = time.monotonic() + 8
                    while second.call('ZRANGE', key, 0, -1, 'WITHSCORES') != (
                            expected):
                        assert time.monotonic() < deadline
                        time.sleep(.02)
                assert second.call('GEOSEARCH', 'geo', 'FROMMEMBER', 'a',
                                   'BYBOX', 0, 0, 'm') == [b'a']
                result = second.call('GEOSEARCHSTORE', 'denied', 'geo',
                                     'FROMMEMBER', 'a', 'BYRADIUS', 1, 'm')
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
            checks = compare(*clients)
        finally:
            for client in clients:
                client.close()
        typed_checks = typed(ports)
        for port in ports:
            watch_and_acl(port)
    persistence(args.pkv)
    replication(args.pkv)
    cluster(args.pkv, False)
    cluster(args.redis, True)
    print('GEO search stable differential assertions:', checks)
    print('GEO search typed differential assertions:', typed_checks)
    print('GEO search indexed range, ANY, stores, TTL, WATCH and EXEC '
          'passed')


if __name__ == '__main__':
    main()
