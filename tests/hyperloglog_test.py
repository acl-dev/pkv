#!/usr/bin/env python3
"""HLL cardinality, binary interoperability, transactions and TTL contracts."""
import argparse
import contextlib
import random
import os
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor
from integration_test import Client, ErrorReply, server


def compare(first, second):
    checked = 0

    def call(*args):
        nonlocal checked
        a, b = first.call(*args), second.call(*args)
        def summary(value):
            if isinstance(value, bytes) and len(value) > 120:
                return (len(value), value[:48], value[-16:])
            return value
        assert type(a) is type(b) and a == b, (
            args[:2], summary(a), summary(b))
        checked += 1
        return a

    call('PFCOUNT', 'missing')
    call('EXISTS', 'missing')
    call('PFADD', b'binary\0\xff', b'value\0\xff')
    call('PFCOUNT', b'binary\0\xff')
    call('PFMERGE', b'dest\0\xff', b'binary\0\xff')
    call('GET', b'dest\0\xff')
    call('PFADD', 'empty')
    call('GET', 'empty')
    call('PFADD', 'empty')
    call('PFCOUNT', 'empty')
    call('GET', 'empty')
    randomizer = random.Random(7143)
    for i in range(20):
        values = [b'', b'\0', b'\xff'] if not i else [
            randomizer.randbytes(randomizer.randrange(1, 40))
            for _ in range(350)]
        call('PFADD', 'a', *values)
        call('GET', 'a')
        call('PFCOUNT', 'a')
        call('GET', 'a')
        call('PFADD', 'a', *values)
    call('PFADD', 'b', 'one', 'two', 'three')
    call('PFCOUNT', 'a', 'b', 'a', 'missing')
    call('PFMERGE', 'out', 'a', 'b')
    call('GET', 'out')
    call('PFCOUNT', 'out')
    call('GET', 'out')
    call('PFMERGE', 'b')
    call('GET', 'b')
    call('PFMERGE', 'out', 'out', 'empty', 'missing')
    call('PFCOUNT', 'out')
    call('PFMERGE', 'fresh', 'missing')
    call('GET', 'fresh')
    call('LPUSH', 'wrongtype', 'x')
    for command in ['PFADD', 'PFCOUNT', 'PFMERGE']:
        call(command, 'wrongtype')
        call(command)
    call('SET', 'bad', 'not-hll')
    call('PFADD', 'bad', 'x')
    call('PFCOUNT', 'bad')
    before = call('GET', 'out')
    call('PFMERGE', 'out', 'a', 'bad')
    assert call('GET', 'out') == before
    call('PFCOUNT', 'a', 'bad')
    header = b'HYLL\1' + b'\0' * 11
    call('SET', 'corrupt', header)
    call('PFCOUNT', 'corrupt')  # Valid cache bypasses corrupt sparse body.
    call('PFADD', 'corrupt')
    call('PFCOUNT', 'corrupt', 'missing')
    call('PFADD', 'corrupt', 'x')
    call('GET', 'corrupt')
    for key in ['empty', 'a', 'b']:
        value = second.call('GET', key)
        call('SET', 'imported', value)
        call('PFADD', 'imported', 'imported-element')
        call('PFCOUNT', 'imported')
        call('GET', 'imported')
    fixtures = [b'HYLL\2' + b'\0' * 11, header + b'\x40',
                header + b'\0', header + b'\x7f\xff\0',
                header + b'\x40\0' * 16384]
    for value in fixtures:
        value = value[:15] + b'\x80' + value[16:]
        call('SET', 'fixture', value)
        call('PFCOUNT', 'fixture')
        call('PFCOUNT', 'fixture', 'missing')
    dense = b'HYLL\0' + b'\0' * 10 + b'\x80' + b'\xff' * 12288
    call('SET', 'fixture', dense)
    call('PFCOUNT', 'fixture')
    call('PFCOUNT', 'fixture', 'missing')
    call('GET', 'fixture')
    call('MULTI')
    call('PFADD', 'exec', 'x')
    call('PFCOUNT', 'exec')
    call('PFMERGE', 'merged', 'exec', 'b')
    call('PFCOUNT', 'merged')
    call('EXEC')
    call('GET', 'merged')
    call('SELECT', 3)
    call('PFCOUNT', 'a')
    call('PFADD', 'a', 'other-db')
    call('PFCOUNT', 'a')
    call('SELECT', 0)
    for command in ['PFADD', 'PFCOUNT', 'PFMERGE']:
        call('COMMAND', 'INFO', command)
    call('COMMAND', 'GETKEYS', 'PFCOUNT', 'a', 'b')
    call('COMMAND', 'GETKEYS', 'PFMERGE', 'out', 'a', 'b')
    return checked


def concurrent(port, reference):
    empty = b'HYLL\0' + b'\0' * (12304 - 5)
    assert reference.call('SET', 'concurrent', empty) == b'OK'
    setup = Client(port)
    try:
        assert setup.call('SET', 'concurrent', empty) == b'OK'
    finally:
        setup.close()

    def worker(index):
        client = Client(port)
        try:
            for i in range(128):
                assert client.call('PFADD', 'concurrent',
                    'worker-%d-%d' % (index, i)) in (0, 1)
        finally:
            client.close()
    with ThreadPoolExecutor(max_workers=8) as pool:
        list(pool.map(worker, range(8)))
    elements = ['worker-%d-%d' % (index, i)
                for index in range(8) for i in range(128)]
    assert reference.call('PFADD', 'concurrent', *elements) == 1
    client = Client(port)
    try:
        assert client.call('PFCOUNT', 'concurrent') == reference.call(
            'PFCOUNT', 'concurrent')
        assert client.call('GET', 'concurrent') == reference.call(
            'GET', 'concurrent')
    finally:
        client.close()


def watch(port):
    first, second = Client(port), Client(port)
    try:
        assert first.call('PFADD', 'watched', 'first') == 1
        assert first.call('WATCH', 'watched') == b'OK'
        assert second.call('PFCOUNT', 'watched') == 1
        assert first.call('MULTI') == b'OK'
        assert first.call('PING') == b'QUEUED'
        assert first.call('EXEC') is None
        for command in [('PFCOUNT', 'watched'),
                        ('PFADD', 'watched', 'first')]:
            assert first.call('WATCH', 'watched') == b'OK'
            assert second.call(*command) in (0, 1)
            assert first.call('MULTI') == b'OK'
            assert first.call('PING') == b'QUEUED'
            assert first.call('EXEC') == [b'PONG']
        assert first.call('PEXPIRE', 'watched', 60000) == 1
        assert second.call('PFADD', 'watched', 'second') == 1
        assert second.call('PFCOUNT', 'watched') == 2
        assert 0 < first.call('PTTL', 'watched') <= 60000
        assert first.call('PFMERGE', 'watched', 'missing') == b'OK'
        assert 0 < first.call('PTTL', 'watched') <= 60000
        assert first.call('WATCH', 'watched') == b'OK'
        assert second.call('PFMERGE', 'watched', 'missing') == b'OK'
        assert first.call('MULTI') == b'OK'
        assert first.call('PING') == b'QUEUED'
        assert first.call('EXEC') is None
        assert first.call('PFADD', 'expired', 'x') == 1
        assert first.call('PEXPIRE', 'expired', 1) == 1
        time.sleep(.01)
        assert first.call('PFCOUNT', 'expired', 'expired', 'missing') == 0
        assert first.call('EXISTS', 'expired') == 0
        assert first.call('PFADD', 'expired-merge', 'x') == 1
        assert first.call('PEXPIRE', 'expired-merge', 1) == 1
        time.sleep(.01)
        assert first.call('PFMERGE', 'expired-merge', 'expired-merge') == b'OK'
        assert first.call('PFCOUNT', 'expired-merge') == 0
        assert first.call('PTTL', 'expired-merge') == -1
    finally:
        first.close()
        second.close()


def permissions(port):
    admin, client = Client(port), Client(port)
    try:
        assert admin.call('PFADD', 'allowed:a', 'x') == 1
        assert admin.call('ACL', 'SETUSER', 'hll-reader', 'on', 'nopass',
                          '+pfcount', '+pfadd', '%R~allowed:*') == b'OK'
        assert client.call('AUTH', 'hll-reader', 'unused') == b'OK'
        assert client.call('PFCOUNT', 'allowed:a') == 1
        assert isinstance(client.call('PFADD', 'allowed:a', 'y'), ErrorReply)
        assert admin.call('ACL', 'SETUSER', 'hll-reader',
                          '%W~allowed:*') == b'OK'
        assert client.call('PFADD', 'allowed:a', 'y') == 1
        assert client.call('PFCOUNT', 'allowed:a') == 2
        assert isinstance(client.call('PFCOUNT', 'allowed:a', 'denied'),
                          ErrorReply)
    finally:
        admin.close()
        client.close()


def persistence(binary):
    if os.environ.get('PKV_TEST_BACKEND', 'mdb') not in ('rdb', 'wdb'):
        return
    with tempfile.TemporaryDirectory(prefix='pkv-hll-restart-') as path:
        with server(binary, storage_path=path) as port:
            client = Client(port)
            try:
                assert client.call('SELECT', 3) == b'OK'
                assert client.call('PFADD', 'persisted', *range(5000)) == 1
                expected = client.call('PFCOUNT', 'persisted')
                raw = client.call('GET', 'persisted')
            finally:
                client.close()
        with server(binary, storage_path=path) as port:
            client = Client(port)
            try:
                assert client.call('SELECT', 3) == b'OK'
                assert client.call('PFCOUNT', 'persisted') == expected
                assert client.call('GET', 'persisted') == raw
                assert client.call('PFADD', 'persisted', 'after-restart') in (
                    0, 1)
            finally:
                client.close()


def cluster(binary, reference):
    with server(binary, redis=reference, cluster=True) as port:
        client = Client(port)
        try:
            for command in ['PFCOUNT', 'PFMERGE']:
                result = client.call(command, '{alpha}a', '{beta}b')
                assert isinstance(result, ErrorReply) and result.startswith(
                    b'CROSSSLOT'), result
            assert client.call('MULTI') == b'OK'
            assert client.call('INFO', 'stats') == b'QUEUED'
            result = client.call('EXEC')
            assert len(result) == 1 and isinstance(result[0], bytes)
            assert result[0].startswith(b'# Stats\r\n'), result
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
                    assert first.call('SET', 'ready', 'yes') == b'OK'
                    if second.call('GET', 'ready') == b'yes':
                        break
                    assert time.monotonic() < deadline
                    time.sleep(.02)

                def await_bytes(keys):
                    expected = first.call('MGET', *keys)
                    deadline = time.monotonic() + 8
                    while second.call('MGET', *keys) != expected:
                        assert time.monotonic() < deadline
                        time.sleep(.02)
                empty = b'HYLL\0' + b'\0' * (12304 - 5)
                assert first.call('SET', 'live', empty) == b'OK'
                assert first.call('PFADD', 'live', 'first', 'second') == 1
                assert first.call('PEXPIREAT', 'live', 4102444800000) == 1
                await_bytes(['live'])
                assert second.call('PFCOUNT', 'live') == 2
                assert first.call('PFADD', 'live', 'third') == 1
                await_bytes(['live'])
                assert first.call('PFCOUNT', 'live') == 3
                await_bytes(['live'])
                assert first.call('MULTI') == b'OK'
                assert first.call('PFADD', 'live', 'fourth') == b'QUEUED'
                assert first.call('PFMERGE', 'union', 'live') == b'QUEUED'
                assert first.call('EXEC') == [1, b'OK']
                await_bytes(['live', 'union'])
                assert second.call('PFCOUNT', 'union') == 4
                assert second.call('PEXPIRETIME', 'live') == 4102444800000
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
            checked = compare(*clients)
            concurrent(ports[0], clients[1])
        finally:
            for client in clients:
                client.close()
        for port in ports:
            watch(port)
            permissions(port)
    persistence(args.pkv)
    replication(args.pkv)
    cluster(args.pkv, False)
    cluster(args.redis, True)
    print('HLL stable differential assertions:', checked)
    print('HLL raw bytes, sparse/dense, cache, TTL, WATCH and EXEC passed')


if __name__ == '__main__':
    main()
