#!/usr/bin/env python3
"""PKV4 negotiated transport and complete mixed-group memory replay over TCP."""
import argparse
import contextlib
import json
import os
import socket
import struct
import time

from integration_test import Client, server
from exec_live_test import Peer, exact

MAGIC = 0x504b5634
HEADER = struct.Struct('!IIIII')


def frame(operation, database=0, key=b'', value=b''):
    return HEADER.pack(MAGIC, operation, database, len(key), len(value)) + (
        key + value)


def batch(peer):
    first = peer.read()
    assert first[:2] == (MAGIC, 5), first[:3]
    roots, pages, _ = struct.unpack('!IIQ', first[4])
    records = [first] + [peer.read() for _ in range(roots + pages)]
    records.append(peer.read())
    assert records[-1][1] == 11
    assert all(r[0] == MAGIC for r in records)
    return records


def encoded(records, database=None):
    return [frame(r[1], r[2] if database is None else database, r[3], r[4])
            for r in records]


def root_after(records, key, previous=None):
    for r in records:
        if r[3] == key and r[1] in (7, 12, 13):
            previous = r[4]
    assert previous is not None
    return previous


def move(source, target, key, root):
    payload = struct.pack('!IIII', target, 0, len(root), 0) + root
    return [frame(16, source, key, payload)]


def envelope(entries):
    body = [record for entry in entries for record in entry]
    return [frame(14, value=struct.pack('!IIQ', len(entries), len(body) + 2,
                                      56 + sum(map(len, body))))] + body + [
                                          frame(15)]


def view(client, kind, key):
    if kind == 'string':
        return client.call('GET', key)
    if kind in ('hash', 'grouped_hash'):
        result = client.call('HGETALL', key)
        return sorted(zip(result[::2], result[1::2]))
    if kind == 'list':
        return client.call('LRANGE', key, 0, -1)
    if kind == 'set':
        return sorted(client.call('SMEMBERS', key))
    return client.call('ZRANGE', key, 0, -1, 'WITHSCORES')


def commands(kind, key):
    blob = b'v' * 70000
    if kind == 'grouped_hash':
        fields = [part for index in range(129)
                  for part in ('field:%013d' % index, 'val')]
        return (('HSET', key, *fields),
                ('HSET', key, 'field:%013d' % 31, 'next'))
    if kind == 'string':
        return ('SET', key, blob), ('SETRANGE', key, 16000, 'changed')
    if kind == 'hash':
        return ('HSET', key, 'blob', blob), ('HSET', key, 'later', 'next')
    if kind == 'list':
        return ('RPUSH', key, blob, 'side'), ('RPUSH', key, 'later')
    if kind == 'set':
        return ('SADD', key, blob, 'side'), ('SADD', key, 'later')
    return ('ZADD', key, 1, blob, 2, 'side'), ('ZADD', key, 3, 'later')


def wait_value(client, kind, key, expected):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if view(client, kind, key) == expected:
            return
        time.sleep(.01)
    raise AssertionError(('ordered value not applied', kind))


def greeting(listener):
    peer, _ = listener.accept()
    peer.settimeout(5)
    line = b''
    while not line.endswith(b'\r\n'):
        line += exact(peer, 1)
    assert line in (b'PKV-REPL 4\r\n', b'PKV-REPL 5\r\n'), line
    peer.sendall(frame(4))
    return peer


def run(binary, backend):
    kinds = ('string', 'hash', 'grouped_hash', 'list', 'set', 'zset')
    with socket.socket() as listener, contextlib.ExitStack() as sockets:
        listener.bind(('127.0.0.1', 0))
        listener.listen(2)
        listener.settimeout(8)
        address = '127.0.0.1:%d' % listener.getsockname()[1]
        with server(binary, backend=backend) as primary_port, \
                server(binary, backend=backend, databases=16,
                       replica_rpc_source=address) as replica_port:
            upstream = sockets.enter_context(greeting(listener))
            peer = Peer(primary_port, 4)
            source, target = Client(primary_port), Client(replica_port)
            try:
                entries, before, after = [], {}, {}
                for kind in kinds:
                    key = kind.encode() + b'\0ordered'
                    create, update = commands(kind, key)
                    assert source.call(*create) is not None
                    initial = batch(peer)
                    root = root_after(initial, key)
                    if kind == 'grouped_hash':
                        assert root[:4] == b'PMR\x0c', root[:4]
                    before[kind] = view(source, kind, key)
                    upstream.sendall(b''.join(encoded(initial)))
                    wait_value(target, kind, key, before[kind])
                    assert source.call(*update) is not None
                    changed = batch(peer)
                    next_root = root_after(changed, key, root)
                    after[kind] = view(source, kind, key)
                    entries += [move(0, 7, key, root), encoded(changed, 7),
                                move(7, 15, key, next_root)]
                group = envelope(entries)
                prefix = b''.join(group[:-1])
                for offset in range(0, len(prefix), 4093):
                    upstream.sendall(prefix[offset:offset + 4093])
                assert target.call('PING') == b'PONG'
                for kind in kinds:
                    key = kind.encode() + b'\0ordered'
                    assert view(target, kind, key) == before[kind]
                assert target.call('SELECT', 15) == b'OK'
                for kind in kinds:
                    assert target.call('TYPE', kind.encode() +
                                       b'\0ordered') == b'none'
                upstream.sendall(group[-1])
                for kind in kinds:
                    wait_value(target, kind, kind.encode() + b'\0ordered',
                               after[kind])
                for selected in (0, 7):
                    assert target.call('SELECT', selected) == b'OK'
                    for kind in kinds:
                        assert target.call('TYPE', kind.encode() +
                                           b'\0ordered') == b'none'
                # A complete but wrong-prefix group must roll back its page
                # delta and close the stream; it must not expose that prefix.
                key = b'string\0ordered'
                erase = [frame(5, 15, value=struct.pack(
                    '!IIQ', 1, 0, 76 + len(key))), frame(8, 15, key),
                    frame(11, 15)]
                invalid = [erase,
                           move(15, 7, b'zset\0ordered', b'wrong-prefix')]
                upstream.sendall(b''.join(envelope(invalid)))
                assert upstream.recv(1) == b''
                upstream.close()
                upstream = sockets.enter_context(greeting(listener))
                assert target.call('SELECT', 15) == b'OK'
                for kind in kinds:
                    result = view(target, kind, kind.encode() + b'\0ordered')
                    assert result == after[kind]
                # Keep a new group incomplete through server teardown.
                upstream.sendall(envelope([move(15, 7, key, b'pending')])[0])
                assert target.call('PING') == b'PONG'
            finally:
                source.close()
                target.close()
                peer.socket.close()
    print('Negotiated PKV4 mixed replay preserves five types and grouped Hash',
          backend)


def fallback(binary, backend):
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        listener.listen(2)
        listener.settimeout(8)
        address = '127.0.0.1:%d' % listener.getsockname()[1]
        with server(binary, backend=backend,
                    replica_rpc_source=address) as port:
            for version in (5, 4, 3):
                peer, _ = listener.accept()
                with peer:
                    peer.settimeout(5)
                    line = b''
                    while not line.endswith(b'\r\n'):
                        line += exact(peer, 1)
                    assert line == ('PKV-REPL %d\r\n' % version).encode()
                    if version >= 4:
                        continue  # A v3-only publisher rejects that request.
                    peer.sendall(HEADER.pack(0x504b5633, 4, 0, 0, 0))
                    key = b'old-peer'
                    value = json.dumps({'T': 'S', 'E': -1,
                                        'D': 'compatible'}).encode()
                    peer.sendall(HEADER.pack(0x504b5633, 1, 0, len(key),
                                             len(value)) + key + value)
                    client = Client(port)
                    try:
                        wait_value(client, 'string', key, b'compatible')
                    finally:
                        client.close()
    print('Explicit v5/v4-to-v3 reconnect fallback passed', backend)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--pkv', required=True)
    args = parser.parse_args()
    backend = os.environ.get('PKV_TEST_BACKEND', 'mdb')
    run(args.pkv, backend)
    fallback(args.pkv, backend)


if __name__ == '__main__':
    main()
