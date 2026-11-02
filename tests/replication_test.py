#!/usr/bin/env python3
"""Bounded pkv legacy replication stream tests, not Redis replication parity."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import socket
import struct
import threading
import time
from integration_test import Client, server

HEADER = struct.Struct('=IIIII')


class Replica:
    def __init__(self, port):
        deadline = time.monotonic() + 3
        while True:
            try:
                self.socket = socket.create_connection(
                    ('127.0.0.1', port + 10000), timeout=2)
                break
            except ConnectionRefusedError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(.01)
        self.buffer = bytearray()

    def close(self, reset=False):
        if reset:
            self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
        self.socket.close()

    def read(self, timeout=3):
        end = time.monotonic() + timeout
        while True:
            if len(self.buffer) >= HEADER.size:
                operation, keylen, vallen, flag, command = HEADER.unpack_from(self.buffer)
                assert operation in (1, 3) and flag == command == 0
                assert keylen <= 65525 and vallen <= 1024000 and (operation != 3 or vallen == 0)
                size = HEADER.size + keylen + vallen
                if len(self.buffer) >= size:
                    key = bytes(self.buffer[HEADER.size:HEADER.size + keylen])
                    value = bytes(self.buffer[HEADER.size + keylen:size])
                    del self.buffer[:size]
                    return operation, key, value
            remaining = end - time.monotonic()
            if remaining <= 0:
                raise TimeoutError('replication frame timed out')
            self.socket.settimeout(remaining)
            try:
                data = self.socket.recv(65536)
            except socket.timeout:
                raise TimeoutError('replication frame timed out')
            assert data, 'replication connection closed unexpectedly'
            self.buffer.extend(data)

    def until(self, key):
        for _ in range(500):
            frame = self.read()
            if frame[1] == key:
                return frame
        raise AssertionError('replication marker not reached')


def run(port):
    client = Client(port)
    first, second = Replica(port), Replica(port)
    peers = [first, second]
    checks = 0
    try:
        # A second subscriber must register while the first is idle. Retry a
        # probe to account for asynchronous accept; preserve partial frames.
        ready = set()
        for attempt in range(100):
            assert client.call('SET', 'rpc:ready', str(attempt)) == b'OK'
            for index, peer in enumerate(peers):
                if index in ready:
                    continue
                try:
                    assert peer.read(.03)[1] == b'rpc:ready'
                    ready.add(index)
                except TimeoutError:
                    pass
            if len(ready) == 2:
                break
        assert len(ready) == 2, 'idle subscriber blocked accepting another replica'
        checks += 1
        assert client.call('SET', 'rpc:marker', 'start') == b'OK'
        for peer in peers:
            assert json.loads(peer.until(b'rpc:marker')[2])['D'] == 'start'
            checks += 1
        value = b'value\0with\r\nbytes'
        for key in (b'', bytes(range(256)), b'rpc:ordinary'):
            assert client.call('SET', key, value, 'PXAT', 4102444800123) == b'OK'
            for peer in peers:
                operation, actual_key, encoded = peer.read()
                record = json.loads(encoded)
                assert operation == 1 and actual_key == key and record['D'].encode() == value
                assert record['P'] == 4102444800123
                checks += 1
            assert client.call('GET', key) == value
            assert client.call('EXISTS', key) == 1
            assert client.call('DEL', key) == 1
            for peer in peers:
                assert peer.read() == (3, key, b'')
                checks += 1
        # Nonzero DBs remain intentionally local until a versioned/negotiated
        # multi-DB protocol exists. Their same-name writes must not leak into DB 0.
        assert client.call('SELECT', 7) == b'OK'
        assert client.call('SET', 'rpc:marker', 'local-only') == b'OK'
        assert client.call('SELECT', 0) == b'OK'
        assert client.call('SET', 'rpc:marker-two', 'db-zero') == b'OK'
        for peer in peers:
            operation, key, encoded = peer.read()
            assert operation == 1 and key == b'rpc:marker-two' and json.loads(encoded)['D'] == 'db-zero'
            checks += 1
        first.close(reset=True)
        peers = [second]
        assert client.call('SET', 'rpc:counter', 0) == b'OK'
        assert json.loads(second.read()[2])['D'] == '0'
        start = threading.Barrier(5)

        def writer(number):
            connection = Client(port)
            try:
                start.wait(timeout=10)
                return [connection.call('INCR', 'rpc:counter') for _ in range(100)]
            finally:
                connection.close()

        def churn():
            start.wait(timeout=10)
            for _ in range(25):
                transient = Replica(port)
                transient.close(reset=True)
            return []

        with ThreadPoolExecutor(max_workers=5) as pool:
            futures = [pool.submit(writer, i) for i in range(4)] + [pool.submit(churn)]
            outcomes = [future.result() for future in futures]
        assert sorted(value for outcome in outcomes for value in outcome) == list(range(1, 401))
        checks += 1
        for expected in range(1, 401):
            operation, key, encoded = second.read()
            assert operation == 1 and key == b'rpc:counter' and json.loads(encoded)['D'] == str(expected), (expected, key, encoded)
            checks += 1
        assert client.call('GET', 'rpc:counter') == b'400'
        assert client.call('PING') == b'PONG'
        checks += 2
    finally:
        client.close()
        for peer in peers:
            peer.close()
    return checks


class VersionedReplica(Replica):
    def __init__(self, port):
        super().__init__(port)
        self.upgraded = False

    def read(self, timeout=3):
        end = time.monotonic() + timeout
        while True:
            if len(self.buffer) >= 20:
                versioned = bytes(self.buffer[:4]) == b'PKV1'
                if versioned:
                    magic, operation, database, keylen, vallen = struct.unpack_from('!IIIII', self.buffer)
                else:
                    operation, keylen, vallen, flag, command = HEADER.unpack_from(self.buffer)
                    assert not self.upgraded and flag == command == 0
                    database = 0
                assert operation in (1, 3, 4) and 0 <= database < 65536
                assert keylen <= 65525 and vallen <= 1024000
                size = 20 + keylen + vallen
                if len(self.buffer) >= size:
                    key = bytes(self.buffer[20:20 + keylen])
                    value = bytes(self.buffer[20 + keylen:size])
                    del self.buffer[:size]
                    if operation == 4:
                        assert versioned and database == keylen == vallen == 0 and not self.upgraded
                        self.upgraded = True
                    return database, operation, key, value
            remaining = end - time.monotonic()
            if remaining <= 0:
                raise TimeoutError('versioned frame timed out')
            self.socket.settimeout(remaining)
            try:
                data = self.socket.recv(65536)
            except socket.timeout:
                raise TimeoutError('replication frame timed out')
            assert data, 'versioned subscription closed'
            self.buffer.extend(data)

    def upgrade(self):
        self.socket.sendall(b'PKV-REPL 1\r\n')
        for _ in range(1000):
            if self.read()[1] == 4:
                return
        raise AssertionError('READY not received')


def run_versioned(port):
    client = Client(port)
    upgraded = VersionedReplica(port)
    legacy = Replica(port)
    checks = 0
    try:
        upgraded.upgrade()
        checks += 1
        # Ensure the legacy client was accepted too, and drain old queued data.
        for attempt in range(100):
            assert client.call('SET', 'v1:barrier', str(attempt)) == b'OK'
            try:
                legacy.until(b'v1:barrier')
                break
            except TimeoutError:
                continue
        else: raise AssertionError('legacy subscription not ready')
        assert client.call('SET', 'v1:begin', 'marker') == b'OK'
        legacy.until(b'v1:begin')
        while upgraded.read()[2] != b'v1:begin': pass
        checks += 1
        for database in (0, 1, 7, 15):
            assert client.call('SELECT', database) == b'OK'
            value = 'database:%d' % database
            assert client.call('SET', b'v1:same\0', value, 'PXAT', 4102444800123) == b'OK'
            dbid, operation, key, encoded = upgraded.read()
            assert (dbid, operation, key) == (database, 1, b'v1:same\0')
            assert json.loads(encoded)['D'] == value and json.loads(encoded)['P'] == 4102444800123
            if database == 0: assert legacy.read()[1] == b'v1:same\0'
            checks += 1
        assert client.call('SELECT', 0) == b'OK'
        assert client.call('COPY', b'v1:same\0', b'v1:same\0', 'DB', 1, 'REPLACE') == 1
        dbid, operation, key, encoded = upgraded.read()
        assert dbid == operation == 1 and key == b'v1:same\0' and json.loads(encoded)['D'] == 'database:0'
        checks += 1
        assert client.call('SELECT', 7) == b'OK'
        assert client.call('RENAME', b'v1:same\0', 'v1:renamed') == b'OK'
        frames = [upgraded.read(), upgraded.read()]
        assert {(dbid, op, key) for dbid, op, key, value in frames} == {(7, 1, b'v1:renamed'), (7, 3, b'v1:same\0')}
        checks += 1
        assert client.call('SELECT', 0) == b'OK'
        assert client.call('SET', 'v1:end', 'marker') == b'OK'
        assert legacy.read()[1] == b'v1:end', 'nonzero DB leaked to legacy subscriber'
        assert upgraded.read()[2] == b'v1:end'
        checks += 1
        invalid = Replica(port)
        try:
            invalid.socket.sendall(b'PKV-REPL 5\r\n')
            invalid.socket.settimeout(3)
            assert invalid.socket.recv(1) == b''
        finally: invalid.close()
        checks += 1
        # Native List pages cannot be flattened into a v1 whole-value frame.
        # Nonzero DB page events disconnect v1, leaving DB-0-only peers alive.
        assert client.call('SELECT', 7) == b'OK'
        assert client.call('RPUSH', 'v1:paged', 'v') == 1
        upgraded.socket.settimeout(3)
        assert upgraded.socket.recv(1) == b''
        assert client.call('LRANGE', 'v1:paged', 0, -1) == [b'v']
        assert client.call('SELECT', 0) == b'OK'
        assert client.call('SET', 'v1:after-pages', 'alive') == b'OK'
        assert legacy.read()[1] == b'v1:after-pages'
        checks += 4
        hash_peer = VersionedReplica(port)
        try:
            hash_peer.upgrade()
            assert client.call('SELECT', 1) == b'OK'
            assert client.call('HSET', 'v1:paged-hash', 'field', 'value') == 1
            hash_peer.socket.settimeout(3)
            assert hash_peer.socket.recv(1) == b''
            assert client.call('HGET', 'v1:paged-hash', 'field') == b'value'
            assert client.call('SELECT', 0) == b'OK'
            assert client.call('SET', 'v1:after-hash', 'alive') == b'OK'
            assert legacy.read()[1] == b'v1:after-hash'
        finally:
            hash_peer.close()
        checks += 5
        set_peer = VersionedReplica(port)
        try:
            set_peer.upgrade()
            assert client.call('SELECT', 1) == b'OK'
            assert client.call('SADD', 'v1:paged-set', 'member') == 1
            set_peer.socket.settimeout(3)
            assert set_peer.socket.recv(1) == b''
            assert client.call('SISMEMBER', 'v1:paged-set', 'member') == 1
            assert client.call('SELECT', 0) == b'OK'
            assert client.call('SET', 'v1:after-set', 'alive') == b'OK'
            assert legacy.read()[1] == b'v1:after-set'
        finally:
            set_peer.close()
        checks += 5
        zset_peer = VersionedReplica(port)
        try:
            zset_peer.upgrade()
            assert client.call('SELECT', 1) == b'OK'
            assert client.call('ZADD', 'v1:paged-zset', 1, 'member') == 1
            zset_peer.socket.settimeout(3)
            assert zset_peer.socket.recv(1) == b''
            assert client.call('ZSCORE', 'v1:paged-zset', 'member') == b'1'
            assert client.call('SELECT', 0) == b'OK'
            assert client.call('SET', 'v1:after-zset', 'alive') == b'OK'
            assert legacy.read()[1] == b'v1:after-zset'
        finally:
            zset_peer.close()
        checks += 5
        string_peer = VersionedReplica(port)
        try:
            string_peer.upgrade()
            assert client.call('SELECT', 1) == b'OK'
            assert client.call('SET', 'v1:paged-string', b'x' * 2000) == b'OK'
            string_peer.socket.settimeout(3)
            assert string_peer.socket.recv(1) == b''
            assert client.call('GET', 'v1:paged-string') == b'x' * 2000
            assert client.call('SELECT', 0) == b'OK'
            assert client.call('SET', 'v1:after-string', 'alive') == b'OK'
            assert legacy.read()[1] == b'v1:after-string'
        finally:
            string_peer.close()
        checks += 5
    finally:
        client.close(); upgraded.close(); legacy.close()
    return checks


def list_apply_cases(source, target):
    # A marker after the page operations establishes FIFO completion on the
    # real source->publisher->socket->receiver path, without initial sync.
    members = [b'a\0b', b'', b'x' * 100000]
    assert source.call('SELECT', 1) == b'OK'
    assert source.call('RPUSH', 'apply:list', *members) == 3
    assert source.call('PEXPIREAT', 'apply:list', 4102444800123) == 1
    assert source.call('COPY', 'apply:list', 'apply:list', 'DB', 7) == 1
    assert source.call('LSET', 'apply:list', 0, 'independent') == b'OK'
    assert source.call('RENAME', 'apply:list', 'apply:renamed') == b'OK'
    assert source.call('LMOVE', 'apply:renamed', 'apply:endpoint',
                       'RIGHT', 'LEFT') == members[-1]
    assert source.call('SELECT', 7) == b'OK'
    assert source.call('SET', 'apply:list-barrier', 'complete') == b'OK'
    assert target.call('SELECT', 7) == b'OK'
    for attempt in range(200):
        if target.call('GET', 'apply:list-barrier') == b'complete':
            break
        time.sleep(.01)
    else:
        raise AssertionError('List page stream did not reach marker')
    assert target.call('LRANGE', 'apply:list', 0, -1) == members
    assert target.call('PEXPIRETIME', 'apply:list') == 4102444800123
    assert target.call('SELECT', 1) == b'OK'
    assert target.call('EXISTS', 'apply:list') == 0
    assert target.call('LRANGE', 'apply:renamed', 0, -1) == [
        b'independent', b'']
    assert target.call('PEXPIRETIME', 'apply:renamed') == 4102444800123
    assert target.call('LRANGE', 'apply:endpoint', 0, -1) == [members[-1]]
    assert target.call('PTTL', 'apply:endpoint') == -1
    return 20


def hash_apply_cases(source, target):
    field, value = b'field\0binary', b'x' * 100000
    assert source.call('SELECT', 7) == b'OK'
    assert source.call('HSET', 'apply:hash', field, value, 'count', '1') == 2
    assert source.call('PEXPIREAT', 'apply:hash', 4102444800123) == 1
    assert source.call('COPY', 'apply:hash', 'apply:hash', 'DB', 1) == 1
    assert source.call('HINCRBY', 'apply:hash', 'count', 5) == 6
    assert source.call('HDEL', 'apply:hash', field) == 1
    assert source.call('RENAME', 'apply:hash', 'apply:renamed-hash') == b'OK'
    assert source.call('HSET', 'apply:renamed-hash', 'new', 'value') == 1
    assert source.call('PERSIST', 'apply:renamed-hash') == 1
    assert source.call('SET', 'apply:hash-barrier', 'complete') == b'OK'
    assert target.call('SELECT', 7) == b'OK'
    for attempt in range(200):
        if target.call('GET', 'apply:hash-barrier') == b'complete':
            break
        time.sleep(.01)
    else:
        raise AssertionError('Hash page stream did not reach marker')
    assert target.call('HGET', 'apply:renamed-hash', 'count') == b'6'
    assert target.call('HGET', 'apply:renamed-hash', 'new') == b'value'
    assert target.call('HLEN', 'apply:renamed-hash') == 2
    assert target.call('EXISTS', 'apply:hash') == 0
    assert target.call('PTTL', 'apply:renamed-hash') == -1
    assert target.call('SELECT', 1) == b'OK'
    assert target.call('HGET', 'apply:hash', field) == value
    assert target.call('HGET', 'apply:hash', 'count') == b'1'
    assert target.call('PEXPIRETIME', 'apply:hash') == 4102444800123
    return 21


def set_apply_cases(source, target):
    members = [b'', b'member\0binary', b'm' * 100000]
    assert source.call('SELECT', 7) == b'OK'
    assert source.call('SADD', 'apply:set', *members) == 3
    assert source.call('PEXPIREAT', 'apply:set', 4102444800123) == 1
    assert source.call('COPY', 'apply:set', 'apply:set', 'DB', 1) == 1
    assert source.call('SMOVE', 'apply:set', 'apply:moved-set', members[2]) == 1
    assert source.call('SUNIONSTORE', 'apply:union-set', 'apply:set',
                       'apply:moved-set') == 3
    assert source.call('RENAME', 'apply:set', 'apply:renamed-set') == b'OK'
    assert source.call('SREM', 'apply:renamed-set', members[1]) == 1
    assert source.call('PERSIST', 'apply:renamed-set') == 1
    assert source.call('SET', 'apply:set-barrier', 'complete') == b'OK'
    assert target.call('SELECT', 7) == b'OK'
    for attempt in range(200):
        if target.call('GET', 'apply:set-barrier') == b'complete':
            break
        time.sleep(.01)
    else:
        raise AssertionError('Set page stream did not reach marker')
    assert target.call('SMEMBERS', 'apply:renamed-set') == [b'']
    assert target.call('SMEMBERS', 'apply:moved-set') == [members[2]]
    assert set(target.call('SMEMBERS', 'apply:union-set')) == set(members)
    assert target.call('EXISTS', 'apply:set') == 0
    assert target.call('PTTL', 'apply:renamed-set') == -1
    assert target.call('SELECT', 1) == b'OK'
    assert set(target.call('SMEMBERS', 'apply:set')) == set(members)
    assert target.call('PEXPIRETIME', 'apply:set') == 4102444800123
    return 19


def zset_apply_cases(source, target):
    member, large = b'member\0binary', b'x' * 100000
    assert source.call('SELECT', 7) == b'OK'
    assert source.call('ZADD', 'apply:zset', '-0', b'', 2, member,
                       'inf', large) == 3
    assert source.call('PEXPIREAT', 'apply:zset', 4102444800123) == 1
    assert source.call('COPY', 'apply:zset', 'apply:zset', 'DB', 1) == 1
    assert source.call('ZINCRBY', 'apply:zset', 3, member) == b'5'
    assert source.call('ZUNIONSTORE', 'apply:union-zset', 2,
                       'apply:zset', 'apply:zset', 'WEIGHTS', 1, 2) == 3
    assert source.call('ZREM', 'apply:zset', large) == 1
    assert source.call('RENAME', 'apply:zset', 'apply:renamed-zset') == b'OK'
    assert source.call('PERSIST', 'apply:renamed-zset') == 1
    assert source.call('ZRANGESTORE', 'apply:range-zset',
                       'apply:renamed-zset', 0, -1) == 2
    assert source.call('ZPOPMIN', 'apply:range-zset') == [b'', b'-0']
    assert source.call('SET', 'apply:zset-barrier', 'complete') == b'OK'
    assert target.call('SELECT', 7) == b'OK'
    for attempt in range(200):
        if target.call('GET', 'apply:zset-barrier') == b'complete':
            break
        time.sleep(.01)
    else:
        raise AssertionError('ZSet page stream did not reach marker')
    assert target.call('ZRANGE', 'apply:renamed-zset', 0, -1,
                       'WITHSCORES') == [b'', b'-0', member, b'5']
    assert target.call('ZCARD', 'apply:union-zset') == 3
    assert target.call('ZSCORE', 'apply:union-zset', member) == b'15'
    assert target.call('ZSCORE', 'apply:union-zset', large) == b'inf'
    assert target.call('ZRANGE', 'apply:range-zset', 0, -1) == [member]
    assert target.call('EXISTS', 'apply:zset') == 0
    assert target.call('PTTL', 'apply:renamed-zset') == -1
    assert target.call('SELECT', 1) == b'OK'
    assert target.call('ZCARD', 'apply:zset') == 3
    assert target.call('ZSCORE', 'apply:zset', member) == b'2'
    assert target.call('ZSCORE', 'apply:zset', b'') == b'-0'
    assert target.call('PEXPIRETIME', 'apply:zset') == 4102444800123
    return 25


def string_apply_cases(source, target):
    assert source.call('SELECT', 7) == b'OK'
    key = b'apply:string\0'
    value = bytes(range(256)) * 512
    assert source.call('SET', key, value, 'PXAT', 4102444800123) == b'OK'
    assert source.call('APPEND', key, b'\0tail') == len(value) + 5
    value += b'\0tail'
    assert source.call('SETRANGE', key, 16383, b'\0cross') == len(value)
    value = value[:16383] + b'\0cross' + value[16389:]
    assert source.call('SETBIT', key, 8 * 16384 + 1, 1) == 1
    assert source.call('SETBIT', key, 8 * 16384 + 1, 0) == 1
    value = value[:16384] + bytes([value[16384] & ~64]) + value[16385:]
    assert source.call('COPY', key, key, 'DB', 1, 'REPLACE') == 1
    assert source.call('RENAME', key, 'apply:renamed-string') == b'OK'
    assert source.call('SET', key, b'small') == b'OK'
    assert source.call('SETBIT', key, 0, 1) == 0
    assert source.call('PERSIST', 'apply:renamed-string') == 1
    assert source.call('BITOP', 'XOR', 'apply:bitop',
                       'apply:renamed-string',
                       'apply:renamed-string') == len(value)
    assert source.call('BITOP', 'NOT', 'apply:bitop',
                       'apply:bitop') == len(value)
    assert source.call('BITOP', 'ONE', key, key, key) == 5
    assert source.call('SET', 'apply:bitop-empty', b'x' * 2000) == b'OK'
    assert source.call('BITFIELD', 'apply:bitop', 'SET', 'i64',
                       16384 * 8 - 3, -9223372036854775808) == [-1]
    assert source.call('BITFIELD', 'apply:field-fail', 'OVERFLOW', 'FAIL',
                       'SET', 'u1', 100, 2) == [None]
    assert source.call('BITOP', 'OR', 'apply:bitop-empty',
                       'apply:missing-bitop') == 0
    assert source.call('SET', 'apply:string-barrier', 'complete') == b'OK'
    assert target.call('SELECT', 7) == b'OK'
    for _ in range(200):
        if target.call('GET', 'apply:string-barrier') == b'complete':
            break
        time.sleep(.01)
    else:
        raise AssertionError('String page stream did not reach marker')
    assert target.call('GET', key) == b'\0' * 5
    assert target.call('BITFIELD_RO', 'apply:bitop', 'GET', 'i64',
                       16384 * 8 - 3) == [-9223372036854775808]
    assert target.call('GET', 'apply:field-fail') == b'\0' * 13
    assert target.call('EXISTS', 'apply:bitop-empty') == 0
    assert target.call('GET', 'apply:renamed-string') == value
    assert target.call('PEXPIRETIME', 'apply:renamed-string') == -1
    assert target.call('SELECT', 1) == b'OK'
    assert target.call('GET', key) == value
    assert target.call('PEXPIRETIME', key) == 4102444800123
    return 29


def run_apply(binary, source_port):
    checks = 0
    with server(binary, replica_rpc_source='127.0.0.1:%d' % (source_port + 10000)) as target_port:
        source, target = Client(source_port), Client(target_port)
        try:
            assert source.call('SELECT', 1) == b'OK' and target.call('SELECT', 1) == b'OK'
            # Wait for a DB 1 marker to prove negotiation and routing, without
            # claiming an initial snapshot or replay of earlier source values.
            for attempt in range(100):
                marker = str(attempt).encode()
                assert source.call('SET', 'apply:ready', marker) == b'OK'
                if target.call('GET', 'apply:ready') == marker: break
                time.sleep(.01)
            else: raise AssertionError('versioned replica did not become ready')
            checks += 1
            expected = {}
            for database in (0, 1, 7, 15):
                assert source.call('SELECT', database) == b'OK'
                value = ('value:%d' % database).encode()
                assert source.call('SET', b'apply:same\0', value, 'PXAT', 4102444800123) == b'OK'
                expected[database] = value
            for database, value in expected.items():
                assert target.call('SELECT', database) == b'OK'
                for attempt in range(100):
                    if target.call('GET', b'apply:same\0') == value: break
                    time.sleep(.01)
                else: raise AssertionError(('replica value missing', database))
                assert target.call('PEXPIRETIME', b'apply:same\0') == 4102444800123
                checks += 2
            assert source.call('SELECT', 0) == b'OK'
            assert source.call('COPY', b'apply:same\0', b'apply:same\0', 'DB', 1, 'REPLACE') == 1
            assert target.call('SELECT', 1) == b'OK'
            for attempt in range(100):
                if target.call('GET', b'apply:same\0') == expected[0]: break
                time.sleep(.01)
            else: raise AssertionError('cross-DB copy did not replicate')
            checks += 1
            assert source.call('SELECT', 7) == b'OK'
            assert source.call('DEL', b'apply:same\0') == 1
            assert target.call('SELECT', 7) == b'OK'
            for attempt in range(100):
                if target.call('GET', b'apply:same\0') is None: break
                time.sleep(.01)
            else: raise AssertionError('DB-scoped delete did not apply')
            assert target.call('SELECT', 0) == b'OK'
            assert target.call('GET', b'apply:same\0') == expected[0]
            checks += 2
            checks += string_apply_cases(source, target)
            checks += list_apply_cases(source, target)
            checks += hash_apply_cases(source, target)
            checks += set_apply_cases(source, target)
            checks += zset_apply_cases(source, target)
        finally: source.close(); target.close()
    return checks


def run_reconnect_and_legacy(binary):
    listener = socket.socket()
    listener.bind(('127.0.0.1', 0))
    listener.listen(3)
    listener.settimeout(5)
    address = '127.0.0.1:%d' % listener.getsockname()[1]
    checks = 0
    def accept():
        nonlocal checks
        peer, _ = listener.accept()
        peer.settimeout(5)
        greeting = bytearray()
        while not greeting.endswith(b'\r\n'):
            data = peer.recv(1)
            assert data and len(greeting) < 64
            greeting.extend(data)
        assert greeting in (b'PKV-REPL 1\r\n', b'PKV-REPL 2\r\n',
                            b'PKV-REPL 3\r\n', b'PKV-REPL 4\r\n')
        checks += 1
        return peer
    def value(client, expected):
        nonlocal checks
        for _ in range(150):
            if client.call('GET', 'reconnect:key') == expected: break
            time.sleep(.01)
        else: raise AssertionError(('replica did not apply', expected))
        checks += 1
    def frame(database, payload):
        key = b'reconnect:key'
        encoded = json.dumps({'T': 'S', 'E': -1, 'D': payload}).encode()
        return struct.pack('!IIIII', 0x504b5631, 1, database, len(key), len(encoded)) + key + encoded
    try:
        with server(binary, replica_rpc_source=address) as target_port:
            client = Client(target_port)
            peers = []
            try:
                peer = accept(); peers.append(peer)
                peer.sendall(struct.pack('!IIIII', 0x504b5631, 4, 0, 0, 0) + frame(1, 'acknowledged'))
                assert client.call('SELECT', 1) == b'OK'
                value(client, b'acknowledged')
                peer.close(); peers.remove(peer)
                peer = accept(); peers.append(peer)
                # No READY on the new connection: stale negotiation state must
                # not authorize this DB 1 update.
                peer.sendall(frame(1, 'unacknowledged'))
                assert peer.recv(1) == b''
                checks += 1
                value(client, b'acknowledged')
                peer.close(); peers.remove(peer)
                peer = accept(); peers.append(peer)
                # An old upstream does not acknowledge the greeting, but keeps
                # sending native DB 0 frames. The new reader must retain that path.
                key = b'reconnect:key'
                encoded = json.dumps({'T': 'S', 'E': -1, 'D': 'legacy'}).encode()
                peer.sendall(HEADER.pack(1, len(key), len(encoded), 0, 0) + key + encoded)
                assert client.call('SELECT', 0) == b'OK'
                value(client, b'legacy')
                assert client.call('SELECT', 1) == b'OK'
                value(client, b'acknowledged')
            finally:
                client.close()
                for peer in peers: peer.close()
    finally: listener.close()
    return checks


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--pkv', required=True)
    args = parser.parse_args()
    with server(args.pkv) as port:
        print('pkv: %d legacy replication stream assertions passed' % run(port))
        print('pkv: %d versioned negotiation assertions passed' % run_versioned(port))
        print('pkv: %d two-process multi-DB apply assertions passed' % run_apply(args.pkv, port))
    print('pkv: %d reconnect/legacy upstream assertions passed' % run_reconnect_and_legacy(args.pkv))
