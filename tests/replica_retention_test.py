#!/usr/bin/env python3
"""Replica reads retain pages needed by delayed authoritative updates."""
import argparse
import os
import socket
import struct
import time

from integration_test import Client, server
from exec_live_test import Peer, frame, greeting, root_batch, await_value


def batch(peer):
    first = peer.read()
    assert first[1] == 5, first
    roots, pages, _ = struct.unpack('!IIQ', first[4])
    packets = [first]
    for _ in range(roots + pages):
        packets.append(peer.read())
    packets.append(peer.read())
    assert packets[-1][1] == 11
    return packets


def encode(packets, database=1):
    return b''.join(frame(item[1], database, item[3], item[4])
                    for item in packets)


def delayed_expiry(binary):
    listener = socket.socket()
    listener.bind(('127.0.0.1', 0))
    listener.listen()
    listener.settimeout(8)
    address = '127.0.0.1:%d' % listener.getsockname()[1]
    try:
        with server(binary) as primary_port:
            source, capture = Client(primary_port), Peer(primary_port)
            try:
                key, original = b'retention\0key', b'x' * 70000
                deadline = int(time.time() * 1000) + 5000
                assert source.call('SET', key, original, 'PXAT',
                                   deadline) == b'OK'
                initial = batch(capture)
                assert source.call('SETRANGE', key, 16384, 'y') == 70000
                delta = batch(capture)
                assert source.call('PERSIST', key) == 1
                persist = batch(capture)
                assert sum(item[1] == 9 for item in initial) == 5
                assert sum(item[1] == 9 for item in delta) == 1
                assert not any(item[1] == 9 for item in persist)
                with server(binary, replica_rpc_source=address) as port:
                    client, downstream = Client(port), Peer(port)
                    peer = greeting(listener)
                    try:
                        peer.sendall(encode(initial) + b''.join(
                            root_batch(b'initial-marker', 'ready')))
                        await_value(client, ['initial-marker'], [b'ready'])
                        assert client.call('SELECT', 1) == b'OK'
                        time.sleep(max(0, deadline / 1000 - time.time()) + .025)
                        assert client.call('GET', key) is None
                        assert client.call('PTTL', key) == -2
                        assert client.call('RANDOMKEY') == key
                        assert client.call('KEYS', '*') == []
                        assert client.call('MULTI') == b'OK'
                        assert client.call('GET', key) == b'QUEUED'
                        assert client.call('PTTL', key) == b'QUEUED'
                        assert client.call('EXEC') == [None, -2]
                        downstream.quiet()
                        peer.sendall(encode(delta) + encode(persist) + b''.join(
                            root_batch(b'final-marker', 'ready')))
                        assert client.call('SELECT', 0) == b'OK'
                        await_value(client, ['final-marker'], [b'ready'])
                        assert client.call('SELECT', 1) == b'OK'
                        expected = original[:16384] + b'y' + original[16385:]
                        assert client.call('GET', key) == expected
                        assert client.call('PTTL', key) == -1
                        downstream.quiet()
                    finally:
                        peer.close()
                        downstream.close()
                        client.close()
            finally:
                capture.close()
                source.close()
    finally:
        listener.close()
    print('Delayed partial pages + PERSIST survive replica expired reads')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--pkv', required=True)
    args = parser.parse_args()
    delayed_expiry(os.path.abspath(args.pkv))


if __name__ == '__main__':
    main()
