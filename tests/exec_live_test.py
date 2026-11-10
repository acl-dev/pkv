#!/usr/bin/env python3
"""Negotiated whole-EXEC publication and atomic memory replica groups."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import os
import socket
import struct
import threading
import time

from integration_test import Client, ErrorReply, server
from page_live_test import encoded as native_string

MAGIC = 0x504b5633
HEADER = struct.Struct('!IIIII')


def exact(connection, size):
    value = b''
    while len(value) < size:
        chunk = connection.recv(size - len(value))
        assert chunk, ('truncated frame', size, len(value))
        value += chunk
    return value


def frame(operation, database=0, key=b'', value=b''):
    return HEADER.pack(MAGIC, operation, database, len(key), len(value)) + (
        key + value)


def root_batch(key, value, database=0):
    return raw_root_batch(key, native_string(value), database)


def raw_root_batch(key, value, database=0):
    root = frame(12, database, key, value)
    return [frame(5, database, value=struct.pack('!IIQ', 1, 0,
                                               56 + len(root))),
            root, frame(11, database)]


def envelope(batches):
    body = [record for batch in batches for record in batch]
    begin = frame(14, value=struct.pack('!IIQ', len(batches), len(body) + 2,
                                      56 + sum(map(len, body))))
    return [begin] + body + [frame(15)]


class Peer:
    def __init__(self, port, version=3):
        deadline = time.monotonic() + 5
        while True:
            try:
                self.socket = socket.create_connection(
                    ('127.0.0.1', port + 10000), 3)
                break
            except ConnectionRefusedError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(.01)
        self.socket.sendall(('PKV-REPL %d\r\n' % version).encode())
        magic, operation, database, key, value = self.read()
        assert magic == 0x504b5630 + version and operation == 4
        assert database == 0 and key == value == b''

    def read(self):
        header = exact(self.socket, 20)
        magic, operation, database, keys, values = HEADER.unpack(header)
        assert keys <= 65525 and values <= 16392, HEADER.unpack(header)
        return (magic, operation, database, exact(self.socket, keys),
                exact(self.socket, values))

    def group(self, expected):
        first = self.read()
        assert first[:4] == (MAGIC, 14, 0, b''), first
        batches, records, size = struct.unpack('!IIQ', first[4])
        assert batches == expected, (batches, expected)
        total = 20 + len(first[3]) + len(first[4])
        nested = 0
        databases = set()
        for index in range(1, records):
            item = self.read()
            assert item[0] == MAGIC, item
            total += 20 + len(item[3]) + len(item[4])
            nested += item[1] == 5
            if item[1] == 5:
                databases.add(item[2])
            if index + 1 == records:
                assert item[1:] == (15, 0, b'', b''), item
            else:
                assert item[1] not in (14, 15), item
        assert nested == batches and total == size, (nested, total, size)
        return databases

    def ordinary(self):
        first = self.read()
        assert first[1] == 5, first
        roots, pages, _ = struct.unpack('!IIQ', first[4])
        for _ in range(roots + pages):
            assert self.read()[1] not in (14, 15)
        assert self.read()[1] == 11

    def quiet(self):
        self.socket.settimeout(.06)
        try:
            value = self.socket.recv(1)
            raise AssertionError(('unexpected event or close', value))
        except socket.timeout:
            pass
        finally:
            self.socket.settimeout(3)

    def close(self):
        self.socket.close()


def publication(binary):
    with server(binary) as port:
        peer, old, one = Peer(port), Peer(port, 2), Peer(port, 1)
        client = Client(port)
        try:
            assert client.call('MULTI') == b'OK'
            commands = [
                ('SET', 'left', '1'), ('SET', 'right', '1'),
                ('LPUSH', 'list', 'x'), ('HSET', 'hash', 'field', 'x'),
                ('SADD', 'set', 'x'), ('ZADD', 'zset', 1, 'x'),
                ('SET', 'ephemeral', 'x'), ('DEL', 'ephemeral'),
                ('GET', 'left'), ('LPUSH', 'left', 'wrong-type'),
                ('SET', 'after-error', 'kept'),
                ('SET', b'large\0key', b'x' * 70000),
            ]
            for command in commands:
                assert client.call(*command) == b'QUEUED'
            peer.quiet()
            replies = client.call('EXEC')
            assert len(replies) == len(commands)
            assert replies[8] == b'1' and isinstance(replies[9], ErrorReply)
            assert replies[10:] == [b'OK', b'OK']
            peer.group(10)
            assert old.socket.recv(1) == b''
            assert one.socket.recv(1) == b''
            # Read-only and empty EXEC need no event/capability upgrade.
            old.close()
            old = Peer(port, 2)
            assert client.call('MULTI') == b'OK'
            assert client.call('GET', 'left') == b'QUEUED'
            assert client.call('EXEC') == [b'1']
            assert client.call('MULTI') == b'OK'
            assert client.call('EXEC') == []
            peer.quiet()
            old.quiet()
            assert client.call('SET', 'ordinary', 'after') == b'OK'
            peer.ordinary()
            old.ordinary()
            assert client.call('MULTI') == b'OK'
            assert client.call('SELECT', 1) == b'QUEUED'
            assert client.call('SET', 'mixed', 'one') == b'QUEUED'
            assert client.call('SELECT', 0) == b'QUEUED'
            assert client.call('SET', 'mixed', 'zero') == b'QUEUED'
            assert client.call('EXEC') == [b'OK'] * 4
            assert peer.group(2) == {0, 1}
            assert old.socket.recv(1) == b''
            assert client.call('GET', 'mixed') == b'zero'
            assert client.call('SELECT', 1) == b'OK'
            assert client.call('GET', 'mixed') == b'one'

        finally:
            client.close()
            for item in (peer, old, one):
                item.close()


def overflow(binary):
    for limits in ({'replica_publish_buffer_limit': 200},
                   {'replica_output_buffer_limit': 200},
                   {'replica_pending_messages': 7}):
        with server(binary, replica_limits=limits) as port:
            peer, client = Peer(port), Client(port)
            try:
                assert client.call('MULTI') == b'OK'
                assert client.call('SET', 'a', 'alpha') == b'QUEUED'
                assert client.call('SET', 'b', 'beta') == b'QUEUED'
                assert client.call('EXEC') == [b'OK', b'OK']
                assert peer.socket.recv(1) == b'', 'overflow emitted a prefix'
                assert client.call('MGET', 'a', 'b') == [b'alpha', b'beta']
            finally:
                peer.close()
                client.close()


def await_value(client, keys, expected, timeout=8):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        actual = client.call('MGET', *keys)
        if actual == expected:
            return
        time.sleep(.01)
    raise AssertionError((keys, actual, expected))


def memory_groups(backend=None):
    return (backend or os.environ.get('PKV_TEST_BACKEND', 'mdb')) in (
        'mdb', 'mdb_tbb')


def coherent_replica(binary, backend=None):
    with server(binary, backend=backend) as port:
        with server(binary, replica_rpc_source='127.0.0.1:%d' % (
                port + 10000), backend=backend) as target_port:
            source, target = Client(port), Client(target_port)
            try:
                # A complete EXEC marker proves negotiated group support,
                # rather than observing a pre-READY legacy root conversion.
                deadline = time.monotonic() + 8
                while time.monotonic() < deadline:
                    assert source.call('MULTI') == b'OK'
                    assert source.call('SET', 'pair-a', 0) == b'QUEUED'
                    assert source.call('SET', 'pair-b', 0) == b'QUEUED'
                    assert source.call('EXEC') == [b'OK', b'OK']
                    if target.call('MGET', 'pair-a', 'pair-b') == [b'0', b'0']:
                        break
                    time.sleep(.02)
                else:
                    raise AssertionError('replica did not subscribe')
                start = threading.Barrier(2)

                def writer():
                    start.wait(timeout=5)
                    for value in range(1, 101):
                        assert source.call('MULTI') == b'OK'
                        assert source.call('SET', 'pair-a', value) == b'QUEUED'
                        assert source.call('SET', 'pair-b', value) == b'QUEUED'
                        assert source.call('EXEC') == [b'OK', b'OK']
                        if value % 10 == 0:
                            assert source.call('SET', 'noise', value) == b'OK'

                def reader():
                    start.wait(timeout=5)
                    for _ in range(500):
                        values = target.call('MGET', 'pair-a', 'pair-b')
                        assert values[0] == values[1], values

                with ThreadPoolExecutor(max_workers=2) as pool:
                    first, second = pool.submit(writer), pool.submit(reader)
                    first.result(timeout=60)
                    second.result(timeout=60)
                await_value(target, ['pair-a', 'pair-b'], [b'100', b'100'])
                assert target.call('WATCH', 'replica-ephemeral') == b'OK'
                assert source.call('MULTI') == b'OK'
                assert source.call('SET', 'replica-ephemeral', 'x') == b'QUEUED'
                assert source.call('DEL', 'replica-ephemeral') == b'QUEUED'
                assert source.call('EXEC') == [b'OK', 1]
                assert source.call('SET', 'history-marker', 'done') == b'OK'
                await_value(target, ['history-marker'], [b'done'])
                assert target.call('MULTI') == b'OK'
                assert target.call('EXEC') is None

                # A transaction wholly in DB 1 is supported and isolated.
                assert source.call('MULTI') == b'OK'
                assert source.call('SELECT', 1) == b'QUEUED'
                assert source.call('SET', 'db-one', 'one') == b'QUEUED'
                assert source.call('EXEC') == [b'OK', b'OK']
                assert target.call('SELECT', 1) == b'OK'
                await_value(target, ['db-one'], [b'one'])
                assert target.call('SELECT', 0) == b'OK'
                assert target.call('GET', 'db-one') is None
                # The primary's existing cross-DB EXEC publication can now
                # be applied as one group by a memory replica.
                if memory_groups(backend):
                    assert source.call('MULTI') == b'OK'
                    assert source.call('SET', 'cross-one', 'one') == b'QUEUED'
                    assert source.call('SELECT', 0) == b'QUEUED'
                    assert source.call('SET', 'cross-zero', 'zero') == b'QUEUED'
                    assert source.call('EXEC') == [b'OK', b'OK', b'OK']
                    await_value(target, ['cross-zero'], [b'zero'])
                    assert target.call('SELECT', 1) == b'OK'
                    assert target.call('GET', 'cross-one') == b'one'
                    assert target.call('SELECT', 0) == b'OK'
            finally:
                source.close()
                target.close()


def greeting(listener):
    peer, _ = listener.accept()
    peer.settimeout(5)
    line = b''
    while not line.endswith(b'\r\n'):
        line += exact(peer, 1)
    assert line in (b'PKV-REPL 3\r\n', b'PKV-REPL 4\r\n',
                    b'PKV-REPL 5\r\n'), line
    peer.sendall(frame(4))
    return peer


def rejected_groups(binary, backend=None):
    listener = socket.socket()
    listener.bind(('127.0.0.1', 0))
    listener.listen()
    listener.settimeout(8)
    address = '127.0.0.1:%d' % listener.getsockname()[1]
    try:
        with server(binary, replica_rpc_source=address,
                    backend=backend) as port:
            client = Client(port)
            peer = greeting(listener)
            try:
                data = envelope([root_batch(b'buffer-a', 'a'),
                                 root_batch(b'buffer-b', 'b')])
                peer.sendall(b''.join(data[:-1]))
                # A normal record inside EXEC must force closure; observing
                # that closure proves the prefix was consumed, not just late.
                peer.sendall(frame(1, key=b'intrusion', value=b'bad'))
                assert peer.recv(1) == b''
                assert client.call('MGET', 'buffer-a', 'buffer-b',
                                   'intrusion') == [None, None, None]
                peer.close()
                peer = greeting(listener)
                invalid_root = b'{"T":"S","E":-1,"D":"old"}'
                old_group = envelope([root_batch(b'native-prefix', 'new'),
                                      raw_root_batch(b'legacy-root', invalid_root)])
                peer.sendall(b''.join(old_group))
                assert peer.recv(1) == b''
                assert client.call('EXISTS', 'native-prefix', 'legacy-root') == 0
                peer.close()
                peer = greeting(listener)
                # Cross-DB data remains buffered until the outer commit.
                mixed = envelope([root_batch(b'cross', 'zero', 0),
                                  root_batch(b'cross', 'one', 1)])
                peer.sendall(b''.join(mixed[:-1]))
                assert client.call('GET', 'cross') is None
                assert client.call('SELECT', 1) == b'OK'
                assert client.call('GET', 'cross') is None
                assert client.call('SELECT', 0) == b'OK'
                if memory_groups(backend):
                    peer.sendall(mixed[-1] + b''.join(
                        root_batch(b'group-marker', 'committed')))
                    await_value(client, ['group-marker'], [b'committed'])
                    assert client.call('GET', 'cross') == b'zero'
                    assert client.call('SELECT', 1) == b'OK'
                    assert client.call('GET', 'cross') == b'one'
                    assert client.call('SELECT', 0) == b'OK'
                    # A later invalid DB rejects the entire group.
                    invalid = envelope([root_batch(b'cross', 'bad-zero', 0),
                                        root_batch(b'cross', 'bad-db', 65535)])
                    peer.sendall(b''.join(invalid))
                    assert peer.recv(1) == b''
                    assert client.call('GET', 'cross') == b'zero'
                    assert client.call('SELECT', 1) == b'OK'
                    assert client.call('GET', 'cross') == b'one'
                    assert client.call('SELECT', 0) == b'OK'
                else:
                    # Disk groups need durable coordination. Keep checking
                    # rejection on the configured native storage backend.
                    peer.sendall(mixed[-1])
                    assert peer.recv(1) == b''
                    assert client.call('GET', 'cross') is None
                    assert client.call('SELECT', 1) == b'OK'
                    assert client.call('GET', 'cross') is None
                    assert client.call('SELECT', 0) == b'OK'
                peer.close()
                peer = greeting(listener)
                peer.sendall(b''.join(data[:-1]))
                peer.close()
                peer = greeting(listener)
                assert client.call('MGET', 'buffer-a', 'buffer-b') == [None] * 2
                peer.sendall(b''.join(data))
                await_value(client, ['buffer-a', 'buffer-b'], [b'a', b'b'])
            finally:
                peer.close()
                client.close()
    finally:
        listener.close()


def source_transfers(binary):
    with server(binary) as port:
        client, watcher = Client(port), Client(port)
        try:
            assert watcher.call('WATCH', 'source-ephemeral') == b'OK'
            assert client.call('MULTI') == b'OK'
            commands = [('SET', 'transfer-a', 'value'),
                        ('RENAME', 'transfer-a', 'transfer-b'),
                        ('COPY', 'transfer-b', 'transfer-c'),
                        ('PEXPIRE', 'transfer-b', 600000),
                        ('DEL', 'transfer-c'),
                        ('SET', 'transfer-c', 'recreated'),
                        ('SET', 'source-ephemeral', 'x'),
                        ('DEL', 'source-ephemeral'),
                        ('SCAN', 0, 'COUNT', 100000, 'TYPE', 'string'),
                        ('MGET', 'transfer-a', 'transfer-b', 'transfer-c')]
            for command in commands:
                assert client.call(*command) == b'QUEUED'
            result = client.call('EXEC')
            assert result[:8] == [b'OK', b'OK', 1, 1, 1, b'OK', b'OK', 1]
            assert result[8][0] == b'0', result[8]
            assert set(result[8][1]) == {b'transfer-b', b'transfer-c'}
            assert result[9] == [None, b'value', b'recreated']
            assert 0 < client.call('PTTL', 'transfer-b') <= 600000
            assert client.call('PTTL', 'transfer-c') == -1
            assert watcher.call('MULTI') == b'OK'
            assert watcher.call('EXEC') is None
            assert client.call('SET', 'post-exec', 'live') == b'OK'
            assert client.call('GET', 'post-exec') == b'live'
        finally:
            client.close()
            watcher.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--pkv', required=True)
    args = parser.parse_args()
    binary = os.path.abspath(args.pkv)
    source_transfers(binary)
    publication(binary)
    overflow(binary)
    rejected_groups(binary)
    coherent_replica(binary)
    if not memory_groups():
        rejected_groups(binary, backend='mdb')
        coherent_replica(binary, backend='mdb')
    print('EXEC live publication, limits, reset and atomic reads passed')


if __name__ == '__main__':
    main()
