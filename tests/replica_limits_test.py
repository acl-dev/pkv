#!/usr/bin/env python3
"""Replica queue limits; independent of Redis replication semantics."""
import argparse
import json
import socket
import struct
import threading
import time
from integration_test import Client, server
from replication_test import Replica, VersionedReplica
from page_live_test import receive, receive_group


class PageReplica(Replica):
    def __init__(self, port):
        super().__init__(port)
        self.socket.sendall(b'PKV-REPL 2\r\n')
        assert receive(self.socket) == (4, 0, b'', b'')

    def read(self, timeout=3):
        self.socket.settimeout(timeout)
        roots, pages = receive_group(self.socket)
        assert len(roots) == 1
        operation, database, key, value = roots[0]
        if operation == 8:
            return 3, key, b''
        assert operation in (7, 12) and value[:4] == b'PSR\1'
        flags, expiry, length, count = struct.unpack('!IQQQ', value[4:32])
        if flags == 1:
            payload = value[32:]
            assert not pages and count == 0
        else:
            assert len(pages) == count
            payload = b''.join(item[3][8:] for item in pages)
        assert len(payload) == length
        return 1, key, json.dumps({'D': payload.decode(), 'P': expiry}).encode()


def subscribe(client, port):
    peer = PageReplica(port)
    for attempt in range(100):
        assert client.call('SET', 'limits:ready', str(attempt)) == b'OK'
        try:
            if peer.read(.03)[1] == b'limits:ready':
                return peer
        except (TimeoutError, socket.timeout):
            pass
    peer.close()
    raise AssertionError('subscriber did not register')


def expect_closed(peer):
    deadline = time.monotonic() + 5
    received = 0
    while time.monotonic() < deadline:
        peer.socket.settimeout(max(.01, deadline - time.monotonic()))
        try:
            data = peer.socket.recv(65536)
        except (ConnectionResetError, BrokenPipeError):
            return
        if not data:
            return
        received += len(data)
        assert received < 32 * 1024 * 1024
    raise AssertionError('over-limit subscriber remained connected')


def test_single_record(binary, stage):
    option = 'replica_%s_buffer_limit' % stage
    with server(binary, replica_limits={option: 2048}) as port:
        client = Client(port)
        first = subscribe(client, port)
        second = subscribe(client, port)
        try:
            assert client.call('SET', 'limits:marker', 'before') == b'OK'
            first.until(b'limits:marker')
            second.until(b'limits:marker')
            value = b'x' * 4096
            assert client.call('SET', 'limits:large', value) == b'OK'
            expect_closed(first)
            expect_closed(second)
            assert client.call('GET', 'limits:large') == value
            assert client.call('PING') == b'PONG'
            fresh = subscribe(client, port)
            try:
                assert client.call('SET', 'limits:after', 'fresh') == b'OK'
                operation, key, encoded = fresh.until(b'limits:after')
                assert operation == 1 and key == b'limits:after'
                assert json.loads(encoded)['D'] == 'fresh'
            finally:
                fresh.close()
        finally:
            client.close()
            first.close()
            second.close()
    return 8


def test_upgrade(binary):
    with server(binary, replica_limits={
        'replica_output_buffer_limit': 19
    }) as port:
        client = Client(port)
        peer = VersionedReplica(port)
        try:
            peer.socket.sendall(b'PKV-REPL 1\r\n')
            expect_closed(peer)
            assert client.call('PING') == b'PONG'
        finally:
            client.close()
            peer.close()
    return 2


def test_inline_legacy_budget(binary):
    with server(binary, replica_limits={
        'replica_output_buffer_limit': 2048
    }) as port:
        client = Client(port)
        legacy = Replica(port)
        current = PageReplica(port)
        try:
            for attempt in range(100):
                assert client.call('SET', 'inline:ready', str(attempt)) == b'OK'
                try:
                    legacy.until(b'inline:ready')
                    current.until(b'inline:ready')
                    break
                except TimeoutError:
                    pass
            else:
                raise AssertionError('legacy inline subscriber not ready')
            value = b'\0' * 512
            assert client.call('SET', 'inline:expanded', value) == b'OK'
            expect_closed(legacy)
            record = current.until(b'inline:expanded')
            assert json.loads(record[2])['D'].encode() == value
            assert client.call('GET', 'inline:expanded') == value
        finally:
            legacy.close(); current.close()
        assert client.call('HSET', 'inline:hash', 'field', 'value') == 1
        unsupported = Replica(port)
        try:
            for attempt in range(100):
                assert client.call('SET', 'inline:ready', str(attempt)) == b'OK'
                try:
                    unsupported.until(b'inline:ready')
                    break
                except TimeoutError:
                    pass
            assert client.call('PEXPIRE', 'inline:hash', 60000) == 1
            expect_closed(unsupported)
            assert client.call('HGET', 'inline:hash', 'field') == b'value'
        finally:
            unsupported.close(); client.close()
    return 8


def test_slow_peer(binary):
    with server(binary, replica_limits={
        'replica_output_buffer_limit': 2 * 1024 * 1024,
        'replica_pending_messages': 4096
    }) as port:
        client = Client(port)
        fast = PageReplica(port)
        slow = PageReplica(port)
        slow.socket.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
        errors = []
        progress = [0]
        ready = threading.Condition()
        count = 1500
        try:
            assert client.call('SET', 'limits:begin', 'start') == b'OK'
            # Register the slow peer, then stop consuming it completely.
            for attempt in range(100):
                assert client.call('SET', 'limits:slow', str(attempt)) == b'OK'
                try:
                    if slow.read(.03)[1] == b'limits:slow': break
                except (TimeoutError, socket.timeout):
                    pass
            else:
                raise AssertionError('slow subscriber did not register')
            assert client.call('SET', 'limits:barrier', 'start') == b'OK'
            fast.until(b'limits:barrier')

            def consume():
                try:
                    for expected in range(count):
                        operation, key, encoded = fast.read(10)
                        assert operation == 1 and key == b'limits:stream'
                        value = json.loads(encoded)['D']
                        assert value.startswith('%04d:' % expected)
                        with ready:
                            progress[0] = expected + 1
                            ready.notify_all()
                except BaseException as error:
                    with ready:
                        errors.append(error)
                        ready.notify_all()

            reader = threading.Thread(target=consume)
            reader.start()
            for sequence in range(count):
                value = ('%04d:' % sequence).encode() + b'x' * 8192
                assert client.call('SET', 'limits:stream', value) == b'OK'
                if (sequence + 1) % 64 == 0:
                    # Bound healthy-reader lag below its output quota even
                    # when the Python consumer loses CPU time. The stalled
                    # subscriber still consumes none of the 12-MiB stream.
                    with ready:
                        assert ready.wait_for(lambda: errors or
                            progress[0] >= sequence + 1, timeout=10), progress
                        assert not errors, errors
            reader.join(timeout=15)
            assert not reader.is_alive() and not errors, errors
            expect_closed(slow)
            assert client.call('PING') == b'PONG'
            assert client.call('GET', 'limits:stream') == value
        finally:
            client.close()
            fast.close()
            slow.close()
    return count + 3


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--pkv', required=True)
    args = parser.parse_args()
    checks = test_single_record(args.pkv, 'output')
    checks += test_single_record(args.pkv, 'publish')
    checks += test_upgrade(args.pkv)
    checks += test_inline_legacy_budget(args.pkv)
    checks += test_slow_peer(args.pkv)
    print('pkv: %d replica queue limit checks passed' % checks)
