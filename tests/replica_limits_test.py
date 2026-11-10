#!/usr/bin/env python3
"""Replica queue limits; independent of Redis replication semantics."""
import argparse
import socket
import threading
import time
from integration_test import Client, server
from replication_test import NativeReplica, VersionedReplica


def subscribe(client, port):
    peer = NativeReplica(port)
    assert client.call('SET', 'limits:ready', 'start') == b'OK'
    assert peer.until(b'limits:ready')[2] == b'start'
    return peer


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
                assert encoded == b'fresh'
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


def test_binary_budget(binary):
    with server(binary, replica_limits={'replica_output_buffer_limit': 2048}) as port:
        client = Client(port)
        peer = subscribe(client, port)
        try:
            value = b'\0' * 512
            assert client.call('SET', 'inline:binary', value) == b'OK'
            assert peer.until(b'inline:binary') == (1, b'inline:binary', value)
            assert client.call('GET', 'inline:binary') == value
        finally:
            peer.close()
            client.close()
    return 3


def test_slow_peer(binary):
    with server(binary, replica_limits={
        'replica_output_buffer_limit': 2 * 1024 * 1024,
        'replica_pending_messages': 4096
    }) as port:
        client = Client(port)
        fast = NativeReplica(port)
        slow = NativeReplica(port)
        slow.socket.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
        errors = []
        progress = [0]
        ready = threading.Condition()
        count = 1500
        try:
            assert client.call('SET', 'limits:begin', 'start') == b'OK'
            # Stop consuming the already registered slow peer.
            assert client.call('SET', 'limits:barrier', 'start') == b'OK'
            fast.until(b'limits:barrier')

            def consume():
                try:
                    for expected in range(count):
                        operation, key, encoded = fast.read(10)
                        assert operation == 1 and key == b'limits:stream'
                        assert encoded.startswith(('%04d:' % expected).encode())
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
    checks += test_binary_budget(args.pkv)
    checks += test_slow_peer(args.pkv)
    print('pkv: %d replica queue limit checks passed' % checks)
