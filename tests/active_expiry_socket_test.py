#!/usr/bin/env python3
"""Physical cleanup is observed using DBSIZE without touching expired keys."""
import argparse
import time
from integration_test import Client, server


def empty(client, expected=0):
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        value = client.call('DBSIZE')
        if value == expected:
            return
        time.sleep(.03)
    raise AssertionError(('active expiry count', value, expected))


def run(port):
    client = Client(port)
    try:
        for database in (0, 1):
            assert client.call('SELECT', database) == b'OK'
            for index in range(100):
                key = b'expire:' + str(index).encode() + b'\0'
                assert client.call('SET', key, b'x' * 2000, 'PX', 150) == b'OK'
        for database in (0, 1):
            assert client.call('SELECT', database) == b'OK'
            empty(client)
        assert client.call('SET', 'renewed', 'old', 'PX', 100) == b'OK'
        assert client.call('SET', 'renewed', 'new', 'PX', 10000) == b'OK'
        assert client.call('SET', 'persisted', 'v', 'PX', 100) == b'OK'
        assert client.call('PERSIST', 'persisted') == 1
        time.sleep(.3)
        assert client.call('DBSIZE') == 2
        assert client.call('GET', 'renewed') == b'new'
        assert client.call('GET', 'persisted') == b'v'
        assert client.call('DEL', 'renewed', 'persisted') == 2
        assert client.call('MULTI') == b'OK'
        assert client.call('SET', 'staged', 'v', 'PX', 150) == b'QUEUED'
        assert client.call('DBSIZE') == b'QUEUED'
        assert client.call('EXEC') == [b'OK', 1]
        empty(client)
    finally:
        client.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--pkv', required=True)
    args = parser.parse_args()
    with server(args.pkv) as port:
        run(port)
    with server(args.pkv, active_expire=False) as port:
        client = Client(port)
        try:
            assert client.call('SET', 'lazy', 'v', 'PX', 100) == b'OK'
            time.sleep(.3)
            assert client.call('DBSIZE') == 1
            assert client.call('GET', 'lazy') is None
            assert client.call('DBSIZE') == 0
        finally:
            client.close()
    with server(args.pkv) as primary:
        source = '127.0.0.1:%d' % (primary + 10000)
        with server(args.pkv, replica_rpc_source=source) as replica:
            client = Client(replica)
            try:
                assert client.call('CONFIG', 'SET', 'replica-read-only',
                                   'no') == b'OK'
                assert client.call('SET', 'local', 'v', 'PX', 100) == b'OK'
                time.sleep(.3)
                assert client.call('DBSIZE') == 1
                client.call('DEL', 'local')
                assert client.call('CONFIG', 'SET', 'replica-read-only',
                                   'yes') == b'OK'
                producer = Client(primary)
                try:
                    value = b'x' * 70000
                    deadline = time.monotonic() + 4
                    while time.monotonic() < deadline:
                        assert producer.call('SET', 'replicated', value,
                                             'PX', 2000) == b'OK'
                        time.sleep(.03)
                        if client.call('GET', 'replicated') == value:
                            break
                    else:
                        raise AssertionError('replication did not become ready')
                    empty(client)
                finally:
                    producer.close()
            finally:
                client.close()
    print('Active expiry physical cleanup and renewal checks passed')


if __name__ == '__main__':
    main()
