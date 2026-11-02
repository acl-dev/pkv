#!/usr/bin/env python3
"""GEO scores, wire values, atomic updates and paged ZSet integration."""
import argparse
import contextlib
import os
import random
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor
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

    for key in ['absent', b'binary\0\xff']:
        call('GEOPOS', key)
        call('GEOHASH', key)
        call('GEOPOS', key, 'missing')
        call('GEOHASH', key, 'missing')
        call('GEODIST', key, 'a', 'b')
    call('GEOADD', 'sicily', 13.361389, 38.115556, 'palermo',
         15.087269, 37.502669, 'catania')
    call('TYPE', 'sicily')
    call('ZRANGE', 'sicily', 0, -1, 'WITHSCORES')
    call('GEOPOS', 'sicily', 'palermo', 'catania', 'missing')
    call('GEOHASH', 'sicily', 'palermo', 'catania', 'missing')
    for unit in [(), ('m',), ('KM',), ('ft',), ('mi',), (b'km\0extra',)]:
        call('GEODIST', 'sicily', 'palermo', 'catania', *unit)
        call('GEODIST', 'sicily', 'palermo', 'palermo', *unit)
    for option in ['NX', 'XX', 'CH', 'NX', 'CH', 'XX']:
        call('GEOADD', 'options', option, 12, 34, 'a', 12, 34, 'a',
             15, 35, 'b')
        call('ZMSCORE', 'options', 'a', 'b')
    call('PEXPIREAT', 'options', 4102444800000)
    call('GEOADD', 'options', 'CH', 'XX', 16, 30, 'a', 17, 31, 'new')
    call('PEXPIRETIME', 'options')
    call('GEOADD', 'options', b'NX\0suffix', 1, 2, b'\0\xff')
    call('GEOPOS', 'options', b'\0\xff')
    for longitude, latitude in [(-180, -85.05112878), (180, 85.05112878),
                                (-180, 85.05112878), (180, -85.05112878),
                                ('-0', '-0'), (0, 0)]:
        call('GEOADD', 'edge', longitude, latitude, 'a')
        call('ZSCORE', 'edge', 'a')
        call('GEOPOS', 'edge', 'a')
        call('GEOHASH', 'edge', 'a')
    randomizer = random.Random(7717)
    for _ in range(80):
        xy = [randomizer.uniform(-180, 180), randomizer.uniform(-85, 85)]
        xy2 = [randomizer.uniform(-180, 180), randomizer.uniform(-85, 85)]
        call('GEOADD', 'random', *xy, 'a', *xy2, 'b')
        call('ZMSCORE', 'random', 'a', 'b')
        call('GEOPOS', 'random', 'a', 'b')
        call('GEOHASH', 'random', 'a', 'b')
        call('GEODIST', 'random', 'a', 'b', 'km')
    for score in ['-inf', '-1', '-0.1', '0', '1.5', '4503599627370495',
                  '4503599627370496', '18446744073709551615', 'inf']:
        call('ZADD', 'ordinary', score, 'a')
        call('GEOPOS', 'ordinary', 'a')
        call('GEOHASH', 'ordinary', 'a')
        call('GEODIST', 'ordinary', 'a', 'a')
    call('SET', 'wrong', 'string')
    for command in [('GEOADD',), ('GEOADD', 'x', 1, 2), ('GEOPOS',),
                    ('GEOHASH',), ('GEODIST', 'x', 'a'),
                    ('GEODIST', 'wrong', 'a', 'b', 'bad-unit'),
                    ('GEODIST', 'wrong', 'a', 'b', 'm', 'extra'),
                    ('GEOADD', 'wrong', 'NX', 'XX', 1, 2, 'a'),
                    ('GEOADD', 'wrong', 'CH', 'CH', 'CH'),
                    ('GEOADD', 'wrong', 'NX', 'NX', 'NX'),
                    ('GEOADD', 'wrong', 'XX', 'NX', 'CH'),
                    ('GEOADD', 'wrong', 'GT', 1, 2, 'a'),
                    ('GEOADD', 'wrong', 181, 0, 'a'),
                    ('GEOADD', 'wrong', 'NaN', 0, 'a'),
                    ('GEOADD', 'wrong', 1, 2, 'a'),
                    ('GEOPOS', 'wrong'), ('GEOHASH', 'wrong'),
                    ('GEODIST', 'wrong', 'a', 'b')]:
        call(*command)
    before = call('ZRANGE', 'sicily', 0, -1, 'WITHSCORES')
    for longitude, latitude in [('bad', '0'), ('', '1'), ('0\0x', '0'),
                                ('1e-999', '0'), ('1e999', '0'),
                                ('inf', '0'), ('0', '-inf'),
                                ('0', '85.051128781'), ('-180.00001', 0)]:
        call('GEOADD', 'sicily', 1, 2, 'staged', longitude, latitude, 'bad')
        assert call('ZRANGE', 'sicily', 0, -1, 'WITHSCORES') == before
    call('SELECT', 3)
    call('GEOADD', 'sicily', 1, 2, 'other-db')
    call('GEOPOS', 'sicily', 'other-db', 'palermo')
    call('SELECT', 0)
    call('MULTI')
    call('GEOADD', 'queued', 10, 20, 'a', 11, 21, 'b')
    call('GEOPOS', 'queued', 'a', 'b')
    call('GEODIST', 'queued', 'a', 'b')
    call('GEOHASH', 'queued', 'a')
    call('EXEC')
    for command in ['GEOADD', 'GEOPOS', 'GEOHASH', 'GEODIST']:
        call('COMMAND', 'INFO', command)
    for args in [('GEOADD', 'key', 1, 2, 'member'),
                 ('GEOPOS', 'key'), ('GEOHASH', 'key', 'a', 'b'),
                 ('GEODIST', 'key', 'a', 'b', 'km')]:
        call('COMMAND', 'GETKEYSANDFLAGS', *args)
    return checked


def typed(ports):
    clients = [RespClient(port) for port in ports]
    count = 0
    try:
        for protocol in [3, 2]:
            for client in clients:
                client.call('HELLO', protocol)
            for args in [('GEOADD', 'typed', 12, 34, 'a', 13, 35, 'b'),
                         ('GEOPOS', 'typed', 'a', 'absent'),
                         ('GEOHASH', 'typed', 'a', 'absent'),
                         ('GEODIST', 'typed', 'a', 'b'),
                         ('GEODIST', 'typed', 'a', 'absent'),
                         ('GEOPOS', 'typed'), ('GEOHASH', 'typed')]:
                a, b = [client.call(*args) for client in clients]
                assert canonical(a) == canonical(b), (args, a, b)
                count += 1
    finally:
        for client in clients:
            client.close()
    return count


def watch_and_acl(port):
    first, second = Client(port), Client(port)
    try:
        assert first.call('GEOADD', 'watched', 1, 2, 'a') == 1
        assert first.call('PEXPIREAT', 'watched', 4102444800000) == 1
        for args, changed in [(('NX', 1, 2, 'a'), False),
                              (('XX', 3, 4, 'missing'), False),
                              (('CH', 1, 2, 'a'), False),
                              (('CH', 2, 3, 'a'), True)]:
            assert first.call('WATCH', 'watched') == b'OK'
            second.call('GEOADD', 'watched', *args)
            assert first.call('MULTI') == b'OK'
            assert first.call('PING') == b'QUEUED'
            assert first.call('EXEC') == (None if changed else [b'PONG'])
        assert first.call('PEXPIRETIME', 'watched') == 4102444800000
        assert first.call('PEXPIRE', 'watched', 1) == 1
        time.sleep(.01)
        assert first.call('GEOPOS', 'watched', 'a') == [None]
        assert first.call('GEOADD', 'watched', 'XX', 1, 2, 'a') == 0
        assert first.call('EXISTS', 'watched') == 0
        assert first.call('ACL', 'SETUSER', 'geo-reader', 'on', 'nopass',
                          '+@geo', '%R~allowed:*') == b'OK'
        assert second.call('AUTH', 'geo-reader', 'unused') == b'OK'
        assert second.call('GEOPOS', 'allowed:x', 'a') == [None]
        assert isinstance(second.call('GEOADD', 'allowed:x', 1, 2, 'a'),
                          ErrorReply)
        assert first.call('ACL', 'SETUSER', 'geo-reader',
                          '%W~allowed:*') == b'OK'
        assert second.call('GEOADD', 'allowed:x', 1, 2, 'a') == 1
        assert isinstance(second.call('GEOPOS', 'denied', 'a'), ErrorReply)
    finally:
        first.close()
        second.close()


def concurrent(port, reference):
    def worker(index):
        client = Client(port)
        try:
            for i in range(40):
                assert client.call('GEOADD', 'concurrent', index, i,
                                   '%d:%d' % (index, i)) == 1
        finally:
            client.close()
    with ThreadPoolExecutor(max_workers=8) as pool:
        list(pool.map(worker, range(8)))
    client = Client(port)
    try:
        arguments = []
        for index in range(8):
            for i in range(40):
                arguments += [index, i, '%d:%d' % (index, i)]
        assert reference.call('GEOADD', 'concurrent', *arguments) == 320
        assert client.call('ZRANGE', 'concurrent', 0, -1, 'WITHSCORES') == (
            reference.call('ZRANGE', 'concurrent', 0, -1, 'WITHSCORES'))
    finally:
        client.close()


def persistence(binary):
    if os.environ.get('PKV_TEST_BACKEND', 'mdb') not in ('rdb', 'wdb'):
        return
    with tempfile.TemporaryDirectory(prefix='pkv-geo-restart-') as path:
        expected = None
        for turn in range(2):
            with server(binary, storage_path=path) as port:
                client = Client(port)
                try:
                    assert client.call('SELECT', 3) == b'OK'
                    if not turn:
                        assert client.call('GEOADD', 'persisted', 1, 2, 'a',
                                           3, 4, 'b') == 2
                        expected = client.call('GEOPOS', 'persisted', 'a', 'b')
                    else:
                        assert client.call('GEOPOS', 'persisted', 'a', 'b') == (
                            expected)
                        assert client.call('GEOADD', 'persisted',
                                           5, 6, 'c') == 1
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
                assert first.call('GEOADD', 'live', 1, 2, 'a', 3, 4, 'b') == (
                    b'QUEUED')
                assert first.call('GEOADD', 'live', 'CH', 5, 6, 'a') == (
                    b'QUEUED')
                assert first.call('EXEC') == [2, 1]
                expected = first.call('ZRANGE', 'live', 0, -1, 'WITHSCORES')
                deadline = time.monotonic() + 8
                while second.call('ZRANGE', 'live', 0, -1, 'WITHSCORES') != (
                        expected):
                    assert time.monotonic() < deadline
                    time.sleep(.02)
                assert second.call('GEOPOS', 'live', 'a', 'b') == (
                    first.call('GEOPOS', 'live', 'a', 'b'))
            finally:
                first.close()
                second.close()


def cluster(binary, reference):
    with server(binary, redis=reference, cluster=True) as port:
        client = Client(port)
        try:
            for args in [('GEOADD', 'remote', 1, 2, 'a'),
                         ('GEOPOS', 'remote', 'a'),
                         ('GEOHASH', 'remote', 'a'),
                         ('GEODIST', 'remote', 'a', 'b'),
                         ('GEOADD', 'remote', 181, 0, 'a'),
                         ('GEODIST', 'remote', 'a', 'b', 'bad-unit'),
                         ('GEODIST', 'remote', 'a', 'b', 'm', 'extra')]:
                result = client.call(*args)
                assert isinstance(result, ErrorReply) and result.startswith(
                    b'CLUSTERDOWN'), result
                assert client.call('PING') == b'PONG'
        finally:
            client.close()


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
            checked = compare(*clients)
            concurrent(ports[0], clients[1])
        finally:
            for client in clients:
                client.close()
        typed_count = typed(ports)
        for port in ports:
            watch_and_acl(port)
    persistence(args.pkv)
    replication(args.pkv)
    cluster(args.pkv, False)
    cluster(args.redis, True)
    print('GEO stable differential assertions:', checked)
    print('GEO typed differential assertions:', typed_count)
    print('GEO atomic ZSet, TTL, WATCH, ACL, EXEC, concurrency and replay '
          'passed')


if __name__ == '__main__':
    main()
