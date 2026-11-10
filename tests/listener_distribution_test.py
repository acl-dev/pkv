#!/usr/bin/env python3
"""Connections must reach distinct workers and retain cross-worker semantics."""
import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
import os
import subprocess
import sys
import threading
import time

from integration_test import Client, server


def balanced_accepts(port):
    """A burst or sequential connect must use all four macOS dispatchers."""
    for burst in (False, True):
        clients = []
        try:
            counts = Counter()
            for _ in range(50):
                client = Client(port)
                clients.append(client)
                if not burst:
                    assert client.call('PING') == b'PONG'
            for index, client in enumerate(clients):
                fields = dict(part.split(b'=', 1) for part in
                              client.call('CLIENT', 'INFO').split())
                counts[int(fields[b'io-thread'])] += 1
                assert fields[b'addr'] == \
                    ('%s:%s' % client.socket.getsockname()).encode()
                assert fields[b'laddr'] == \
                    ('%s:%s' % client.socket.getpeername()).encode()
                client.socket.sendall(Client.encode(
                    ('INCR', 'dispatch:%s:%d' % (burst, index))) * 1024)
                assert [client.read() for _ in range(1024)] == \
                    list(range(1, 1025))
            assert len(counts) == 4 and sorted(counts.values()) == \
                [12, 12, 13, 13], counts
            for client in clients[1:]:
                identity = client.call('CLIENT', 'ID')
                assert clients[0].call('CLIENT', 'KILL', 'ID', identity) == 1
                assert client.stream.read(1) == b''
        finally:
            for client in clients:
                client.close()
    # Disconnect before a request is processed, then verify queues keep serving.
    for _ in range(4):
        clients = [Client(port) for _ in range(64)]
        for client in clients:
            client.close()
    client = Client(port)
    try:
        assert client.call('PING') == b'PONG'
    finally:
        client.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--pkv', required=True)
    args = parser.parse_args()
    if sys.platform == 'darwin':
        # Extra inherited descriptors reproduce a dispatch wakeup failure even
        # without an injected library. Exercise a cold burst before warmup.
        descriptors = (*os.pipe(), *os.pipe())
        try:
            with server(args.pkv, fiber_threads=4,
                        inherited_fds=(descriptors[0], descriptors[3]),
                        stdin=subprocess.DEVNULL,
                        startup_env={'MASTER_SERVICE': 'pkv-test-parent'}) as port:
                barrier = threading.Barrier(50)

                def burst(index):
                    barrier.wait()
                    client = Client(port)
                    try:
                        client.socket.settimeout(15)
                        payload = Client.encode(('INCR', 'fd-burst:%d' % index))
                        client.socket.sendall(payload * 4096)
                        assert [client.read() for _ in range(4096)] == \
                            list(range(1, 4097))
                    finally:
                        client.close()

                with ThreadPoolExecutor(max_workers=50) as workers:
                    list(workers.map(burst, range(50)))
        finally:
            for descriptor in descriptors:
                os.close(descriptor)
    clients = []
    with server(args.pkv, fiber_threads=4) as port:
        if sys.platform == 'darwin':
            balanced_accepts(port)
        try:
            owners = {}
            deadline = time.monotonic() + 5
            while len(owners) < 4 and len(clients) < 256 and \
                    time.monotonic() < deadline:
                base = len(clients)
                barrier = threading.Barrier(16)

                def connect(index):
                    barrier.wait()
                    client = Client(port)
                    client.socket.settimeout(10)
                    name = 'listener:%d' % index
                    assert client.call('CLIENT', 'SETNAME', name) == b'OK'
                    return client

                with ThreadPoolExecutor(max_workers=16) as workers:
                    clients.extend(workers.map(connect, range(base, base + 16)))
                entries = clients[0].call('CLIENT', 'LIST').splitlines()
                for entry in entries:
                    fields = dict(item.split(b'=', 1)
                                  for item in entry.split())
                    name = fields.get(b'name', b'')
                    if name.startswith(b'listener:'):
                        index = int(name.split(b':')[1])
                        owners[int(fields[b'io-thread'])] = clients[index]
                if len(owners) < 4:
                    time.sleep(.02)
            assert len(owners) == 4, ('worker distribution', len(owners))
            selected = list(owners.values())
            first, second = selected[:2]
            assert first.call('SET', 'counter', 0, 'PX', 60000) == b'OK'

            def increment(client):
                client.socket.sendall(Client.encode(('INCR', 'counter')) * 1000)
                return [client.read() for _ in range(1000)]

            with ThreadPoolExecutor(max_workers=4) as workers:
                replies = list(workers.map(increment, selected))
            assert sorted(value for row in replies for value in row) == \
                list(range(1, 4001))
            assert first.call('GET', 'counter') == b'4000'
            assert 0 < first.call('PTTL', 'counter') <= 60000
            assert first.call('WATCH', 'counter') == b'OK'
            assert second.call('INCR', 'counter') == 4001
            assert first.call('MULTI') == b'OK'
            assert first.call('SET', 'peer', 'unpublished') == b'QUEUED'
            assert first.call('EXEC') is None
            assert second.call('GET', 'peer') is None
            first.socket.sendall(Client.encode(('BLPOP', 'queue', 2)))
            assert second.call('RPUSH', 'queue', 'value') == 1
            assert first.read() == [b'queue', b'value']
            assert first.call('SUBSCRIBE', 'channel') == \
                [b'subscribe', b'channel', 1]
            assert second.call('PUBLISH', 'channel', 'message') == 1
            assert first.read() == [b'message', b'channel', b'message']
            assert first.call('UNSUBSCRIBE', 'channel') == \
                [b'unsubscribe', b'channel', 0]
            print('Four observed workers: INCR, TTL, WATCH, blocking, pubsub')
        finally:
            for client in clients:
                client.close()


if __name__ == '__main__':
    main()
