#!/usr/bin/env python3
"""Live PKV2 receiver/negotiation; not Redis replication parity."""
import argparse
import json
import socket
import struct
import time
from integration_test import Client, server

HEADER = struct.Struct('!IIIII')
KEYS = (b'', b'txn:\0b')


def frame(operation, database=0, key=b'', value=b'', version=2):
    magic = 0x504b5630 + version
    return HEADER.pack(magic, operation, database, len(key), len(value)) + \
        key + value


def encoded(value):
    return json.dumps({'T': 'S', 'E': -1, 'D': value}).encode()


def transaction(database, roots, pages):
    records = [frame(operation, database, key, value)
               for operation, key, value in roots]
    records += [frame(operation, database, key, struct.pack('!Q', number) +
                      value) for operation, key, number, value in pages]
    size = 56 + sum(map(len, records))
    begin = frame(5, database, value=struct.pack('!IIQ', len(roots),
                                               len(pages), size))
    return [begin] + records + [frame(11, database)]


def update(value):
    roots = [(12 if key == KEYS[0] else 7, key, encoded(value))
             for key in KEYS]
    pages = [(9, key, 1, b'x\0' * 8192) for key in KEYS]
    return transaction(7, roots, pages)


def wait_value(client, expected, *command):
    for _ in range(200):
        actual = client.call(*command)
        if actual == expected:
            return
        time.sleep(.01)
    raise AssertionError((command, actual, expected))


def closed(peer):
    try:
        assert peer.recv(1) == b'', 'invalid stream was retained'
    except ConnectionResetError:
        pass
    peer.close()


class Upstream:
    def __init__(self):
        self.listener = socket.socket()
        self.listener.bind(('127.0.0.1', 0))
        self.listener.listen(4)
        self.listener.settimeout(5)
        self.address = '127.0.0.1:%d' % self.listener.getsockname()[1]
        self.peers = []

    def accept(self, version=5, ready=True):
        peer, _ = self.listener.accept()
        self.peers.append(peer)
        peer.settimeout(4)
        request = bytearray()
        while not request.endswith(b'\r\n'):
            data = peer.recv(1)
            assert data and len(request) < 64
            request.extend(data)
        assert request == ('PKV-REPL %d\r\n' % version).encode(), request
        if ready:
            # This fake upstream supports PKV2 and can select a lower version.
            peer.sendall(frame(4, version=min(version, 2)))
        return peer

    def close(self):
        for peer in self.peers:
            peer.close()
        self.listener.close()


def receive(peer):
    def exact(size):
        out = bytearray()
        while len(out) < size:
            data = peer.recv(size - len(out))
            assert data, 'subscriber closed before complete frame'
            out.extend(data)
        return bytes(out)
    magic, operation, database, key_size, value_size = HEADER.unpack(exact(20))
    assert magic == 0x504b5632
    assert key_size <= 65525 and value_size <= 1024000
    return operation, database, exact(key_size), exact(value_size)


def receive_group(peer):
    operation, database, key, value = receive(peer)
    assert operation == 5 and not key and len(value) == 16
    root_count, page_count, total = struct.unpack('!IIQ', value)
    roots, pages = [], []
    measured = 56
    for index in range(root_count + page_count):
        item = receive(peer)
        op, dbid, name, payload = item
        assert dbid == database
        measured += 20 + len(name) + len(payload)
        if index < root_count:
            assert op in (6, 7, 8, 12) and len(payload) <= 16384
            roots.append(item)
        else:
            assert op in (9, 10) and 8 <= len(payload) <= 16392
            pages.append(item)
    assert receive(peer) == (11, database, b'', b'')
    assert measured == total
    return roots, pages


def test_subscriber(port):
    client = Client(port)
    peer = None
    try:
        short = b'0123456789abcdef'
        assert client.call('SET', 'subscriber:short', short) == b'OK'
        paged = bytes(range(256)) * 273 + b'tail'
        deadline = 4102444800123
        assert client.call('SET', 'subscriber:paged', paged, 'PXAT', deadline) == b'OK'
        peer = socket.create_connection(('127.0.0.1', port + 10000), timeout=3)
        peer.sendall(b'PKV-REPL 2\r\nPKV-REPL 2\r\n')
        assert receive(peer) == (4, 0, b'', b'')
        assert client.call('COPY', 'subscriber:short', 'subscriber:copy') == 1
        roots, pages = receive_group(peer)
        assert len(roots) == 1 and roots[0][2] == b'subscriber:copy' and not pages
        assert roots[0][3] == b'PSR\2\0\0\x10' + short
        copied = b'subscriber:paged-copy'
        assert client.call('COPY', 'subscriber:paged', copied) == 1
        roots, pages = receive_group(peer)
        assert roots == [(12, 0, copied,
                          struct.pack('!4sIQQQ', b'PSR\1', 0, deadline, len(paged), 5))]
        assert pages == [(9, 0, copied, struct.pack('!Q', i + 1) + paged[offset:offset + 16384])
                         for i, offset in enumerate(range(0, len(paged), 16384))]
        assert client.call('GET', copied) == paged
        assert client.call('SETRANGE', copied, 0, b'X\0') == len(paged)
        updated = b'X\0' + paged[2:]
        roots, pages = receive_group(peer)
        assert roots == [(7, 0, copied,
                          struct.pack('!4sIQQQ', b'PSR\1', 0, deadline, len(updated), 5))]
        assert pages == [(9, 0, copied, struct.pack('!Q', 1) + updated[:16384])]
        assert client.call('APPEND', copied, b'\0Y') == len(updated) + 2
        updated += b'\0Y'
        roots, pages = receive_group(peer)
        assert roots == [(7, 0, copied,
                          struct.pack('!4sIQQQ', b'PSR\1', 0, deadline, len(updated), 5))]
        assert pages == [(9, 0, copied, struct.pack('!Q', 5) + updated[65536:])]
        assert client.call('GET', copied) == updated
        assert client.call('SETBIT', copied, 0, 1) == 0
        updated = bytes([updated[0] | 0x80]) + updated[1:]
        roots, pages = receive_group(peer)
        root = struct.pack('!4sIQQQ', b'PSR\1', 0, deadline, len(updated), 5)
        assert roots == [(7, 0, copied, root)]
        assert pages == [(9, 0, copied, struct.pack('!Q', 1) + updated[:16384])]
        boundary = 16383
        old = int.from_bytes(updated[boundary:boundary+3], 'big') >> 4 & 0xffff
        assert client.call('BITFIELD', copied, 'SET', 'u16', boundary * 8 + 4,
                           0xffff) == [old]
        patch = bytes([updated[boundary] | 0x0f, 0xff, updated[boundary+2] | 0xf0])
        updated = updated[:boundary] + patch + updated[boundary+3:]
        roots, pages = receive_group(peer)
        assert roots == [(7, 0, copied, root)]
        assert pages == [(9, 0, copied, struct.pack('!Q', 1) + updated[:16384]),
                         (9, 0, copied, struct.pack('!Q', 2) + updated[16384:32768])]
        assert client.call('GET', copied) == updated
        assert client.call('SELECT', 7) == b'OK'
        assert client.call('SET', 'subscriber:marker', 'new') == b'OK'
        roots, pages = receive_group(peer)
        assert roots[0][:3] == (12, 7, b'subscriber:marker') and not pages
        assert roots[0][3] == b'PSR\2\0\0\3new'
        grouped_key = b'subscriber:grouped-list'
        seed = b's' * 700
        assert client.call('RPUSH', grouped_key, seed, seed) == 2
        receive_group(peer)
        appended = [b'a' * 700, b'b' * 700]
        client.socket.sendall(b''.join(client.encode(('RPUSH', grouped_key, value))
                                      for value in appended))
        assert [client.read(), client.read()] == [3, 4]
        for value in appended:
            roots, pages = receive_group(peer)
            assert len(roots) == 1 and roots[0][2] == grouped_key
            assert any(item[0] == 9 and item[2] == grouped_key and
                       value in item[3] for item in pages)
        assert client.call('LRANGE', grouped_key, 0, -1) == [seed, seed] + appended
        members = [b'', b'a\0b', b'x' * 100000]
        assert client.call('RPUSH', 'subscriber:list', *members) == 3
        roots, pages = receive_group(peer)
        assert len(roots) == 1 and roots[0][:3] == (
            12, 7, b'subscriber:list')
        assert roots[0][3][:4] == b'PLR\2' and len(roots[0][3]) == 48
        assert len(pages) > 1
        assert all(item[0] == 9 and item[2] == b'subscriber:list'
                   for item in pages)
        assert client.call('LRANGE', 'subscriber:list', 0, -1) == members
        assert client.call('PEXPIREAT', 'subscriber:list', 4102444800123) == 1
        roots, pages = receive_group(peer)
        assert roots[0][:3] == (7, 7, b'subscriber:list') and not pages
        assert struct.unpack_from('!Q', roots[0][3], 8)[0] == 4102444800123
        assert client.call('HSET', 'subscriber:hash', 'field',
                           b'v' * 100000, 'count', '1') == 2
        roots, pages = receive_group(peer)
        assert roots[0][:3] == (12, 7, b'subscriber:hash')
        assert roots[0][3][:6] == b'PMR\4\1\0' and len(roots[0][3]) == 12
        assert len(pages) > 2
        assert client.call('PEXPIREAT', 'subscriber:hash', 4102444800123) == 1
        roots, pages = receive_group(peer)
        assert roots[0][:3] == (7, 7, b'subscriber:hash') and not pages
        assert roots[0][3][:6] == b'PMR\4\1\1' and len(roots[0][3]) == 20
        assert struct.unpack_from('!Q', roots[0][3], 12)[0] == 4102444800123
        assert client.call('HINCRBYFLOAT', 'subscriber:hash', 'count',
                           '.125') == b'1.125'
        roots, pages = receive_group(peer)
        assert roots[0][:3] == (7, 7, b'subscriber:hash')
        assert 0 < len(pages) < 10
        assert all(len(item[3]) <= 16384 + 8 for item in pages)
        assert any(b'1.125' in item[3] for item in pages)
        assert client.call('PEXPIRETIME', 'subscriber:hash') == 4102444800123
        assert client.call('INCRBYFLOAT', 'subscriber:float',
                           '.125') == b'0.125'
        roots, pages = receive_group(peer)
        assert roots[0][:3] == (12, 7, b'subscriber:float') and not pages
        assert roots[0][3] == b'PSR\2\0\0\x050.125'
        assert client.call('SADD', 'subscriber:set', *members) == 3
        roots, pages = receive_group(peer)
        assert roots[0][:3] == (12, 7, b'subscriber:set')
        assert roots[0][3][:6] == b'PMR\4\2\0' and len(roots[0][3]) == 12
        assert len(pages) > 2
        assert client.call('PEXPIREAT', 'subscriber:set', 4102444800123) == 1
        roots, pages = receive_group(peer)
        assert roots[0][:3] == (7, 7, b'subscriber:set') and not pages
        assert roots[0][3][:6] == b'PMR\4\2\1' and len(roots[0][3]) == 20
        assert struct.unpack_from('!Q', roots[0][3], 12)[0] == 4102444800123
        assert client.call('ZADD', 'subscriber:zset', '-0', b'', 2,
                           b'a\0b', 'inf', b'x' * 100000) == 3
        roots, pages = receive_group(peer)
        assert roots[0][:3] == (12, 7, b'subscriber:zset')
        assert roots[0][3][:5] == b'PZR\2\0' and len(roots[0][3]) == 13
        assert len(pages) > 2
        assert client.call('PEXPIREAT', 'subscriber:zset', 4102444800123) == 1
        roots, pages = receive_group(peer)
        assert roots[0][:3] == (7, 7, b'subscriber:zset') and not pages
        assert roots[0][3][:5] == b'PZR\2\1' and len(roots[0][3]) == 21
        assert struct.unpack_from('!Q', roots[0][3], 13)[0] == 4102444800123
        pairs = [item for score in range(16) for item in (score, f'm{score:05d}')]
        assert client.call('ZADD', 'subscriber:compact-zset', *pairs) == 16
        roots, pages = receive_group(peer)
        assert roots[0][:3] == (12, 7, b'subscriber:compact-zset')
        if roots[0][3].startswith(b'{'):
            assert not pages
            assert json.loads(roots[0][3]) == {
                'V': 1, 'T': 'Z', 'E': -1, 'P': -1,
                'D': [[f'm{score:05d}', str(score)] for score in range(16)],
            }
        else:
            assert any(item[3][8:12] == b'PMN\x0d' and len(item[3]) == 30
                       for item in pages)
        peer.sendall(b'PKV-REPL 1\r\n')
        closed(peer)
    finally:
        if peer is not None:
            peer.close()
        client.close()


def run(binary):
    upstream = Upstream()
    checks = 0
    try:
        with server(binary, replica_rpc_source=upstream.address) as port:
            client = Client(port)
            try:
                peer = upstream.accept()
                checks += 1
                assert client.call('CONFIG', 'SET', 'replica-read-only',
                                   'no') == b'OK'
                for database in (0, 7):
                    assert client.call('SELECT', database) == b'OK'
                    assert client.call('MSET', KEYS[0], 'before', KEYS[1],
                                       'before') == b'OK'
                assert client.call('CONFIG', 'SET', 'replica-read-only',
                                   'yes') == b'OK'
                peer.sendall(frame(1, 0, b'normal:v2', encoded('plain')))
                assert client.call('SELECT', 0) == b'OK'
                wait_value(client, b'plain', 'GET', 'normal:v2')
                checks += 1
                assert client.call('SELECT', 7) == b'OK'
                records = update('after')
                peer.sendall(b''.join(records[:-1]))
                time.sleep(.05)
                assert client.call('MGET', *KEYS) == [b'before', b'before']
                peer.sendall(records[-1])
                wait_value(client, [b'after', b'after'], 'MGET', *KEYS)
                assert client.call('SELECT', 0) == b'OK'
                assert client.call('MGET', *KEYS) == [b'before', b'before']
                checks += 3
                assert client.call('SELECT', 7) == b'OK'
                # Valid framing with an absent ROOT_KEEP base must fail
                # atomically, including the other root's proposed replacement.
                bad = transaction(7, [(7, KEYS[0], encoded('failed')),
                                     (6, b'z:missing', b'')],
                                  [(9, b'z:missing', 1, b'x')])
                peer.sendall(b''.join(bad))
                closed(peer)
                assert client.call('MGET', *KEYS) == [b'after', b'after']
                checks += 1
                peer = upstream.accept()
                prefix = update('discarded')
                peer.sendall(b''.join(prefix[:-1]))
                peer.close()
                peer = upstream.accept()
                assert client.call('MGET', *KEYS) == [b'after', b'after']
                peer.sendall(b''.join(update('reconnected')))
                wait_value(client, [b'reconnected', b'reconnected'],
                           'MGET', *KEYS)
                checks += 2
                # A normal record cannot interrupt an unfinished transaction.
                peer.sendall(update('interleaved')[0] +
                             frame(1, 7, b'interleaved', encoded('invalid')))
                closed(peer)
                assert client.call('GET', 'interleaved') is None
                assert client.call('MGET', *KEYS) == [b'reconnected'] * 2
                checks += 2
                # Model a v1 server rejecting v5, v4, v3 and v2 requests.
                peer = upstream.accept(ready=False)
                peer.close()
                peer = upstream.accept(version=4, ready=False)
                peer.close()
                peer = upstream.accept(version=3, ready=False)
                peer.close()
                peer = upstream.accept(version=2, ready=False)
                peer.close()
                peer = upstream.accept(version=1)
                peer.sendall(frame(1, 7, b'fallback', encoded('v1'), version=1))
                wait_value(client, b'v1', 'GET', 'fallback')
                checks += 2
                # PKV2 must remain unavailable on the negotiated v1 stream.
                peer.sendall(b''.join(update('wrong-version')))
                closed(peer)
                assert client.call('MGET', *KEYS) == [b'reconnected'] * 2
                checks += 1
                peer = upstream.accept()
                oversized = struct.pack('!IIQ', 1, 0, 64 * 1024 * 1024 + 1)
                peer.sendall(frame(5, 7, value=oversized))
                closed(peer)
                assert client.call('MGET', *KEYS) == [b'reconnected'] * 2
                checks += 1
                assert client.call('CONFIG', 'SET', 'replica-read-only',
                                   'no') == b'OK'
                try:
                    test_subscriber(port)
                finally:
                    assert client.call('CONFIG', 'SET', 'replica-read-only',
                                       'yes') == b'OK'
                checks += 5
                assert client.call('PING') == b'PONG'
                checks += 1
            finally:
                client.close()
    finally:
        upstream.close()
    return checks


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--pkv', required=True)
    args = parser.parse_args()
    print('pkv: %d live PKV2 scenarios passed' % run(args.pkv))
