#!/usr/bin/env python3
"""Normal signals must drain work, close native stores and exit successfully."""
import argparse
import os
from pathlib import Path
import signal
import socket
import tempfile

from integration_test import Client, server


def values(client, write):
    large = b'p' * 65536
    for database in (0, 7):
        assert client.call('SELECT', database) == b'OK'
        if write:
            assert client.call('SET', 'string', large) == b'OK'
            assert client.call('HSET', 'hash', 'field', large) == 1
            assert client.call('RPUSH', 'list', large) == 1
            assert client.call('SADD', 'set', large) == 1
            assert client.call('ZADD', 'zset', 1, large) == 1
        assert client.call('GET', 'string') == large
        assert client.call('HGET', 'hash', 'field') == large
        assert client.call('LINDEX', 'list', 0) == large
        assert client.call('SISMEMBER', 'set', large) == 1
        assert client.call('ZRANGE', 'zset', 0, -1) == [large]


def stopped(binary, backend, active, received, storage):
    info = {}
    with server(binary, backend=backend, databases=16,
                active_expire=active, storage_path=storage,
                process_info=info) as port:
        client = Client(port)
        blocking = Client(port)
        try:
            values(client, True)
            blocking.socket.sendall(Client.encode(('BLPOP', 'missing', 0)))
            os.kill(info['pid'], received)
            result = info['process'].wait(timeout=5)
            assert result == 0, (backend, active, received, result)
            for selected in (port, port + 10000):
                with socket.socket() as connection:
                    connection.settimeout(.5)
                    assert connection.connect_ex(('127.0.0.1', selected))
        finally:
            blocking.close()
            client.close()
    if backend in ('rdb', 'wdb'):
        with server(binary, backend=backend, databases=16,
                    storage_path=storage) as port:
            client = Client(port)
            try:
                values(client, False)
            finally:
                client.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--pkv', required=True)
    args = parser.parse_args()
    backend = os.environ.get('PKV_TEST_BACKEND', 'mdb')
    for active in (False, True):
        for received in (signal.SIGTERM, signal.SIGINT):
            with tempfile.TemporaryDirectory(prefix='pkv-exit-') as root:
                stopped(args.pkv, backend, active, received,
                        Path(root) / 'storage')
    print('Normal SIGTERM/SIGINT exit, blocked clients and restart passed',
          backend)


if __name__ == '__main__':
    main()
