#!/usr/bin/env python3
"""Large trim capacity, EXEC and live native replica regression."""
import argparse
import time
from integration_test import Client, server


def wait_for(client, command, expected, seconds=30):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        actual = client.call(*command)
        if actual == expected:
            return
        time.sleep(.01)
    raise AssertionError((command, expected, actual))


def run(binary, backend):
    value = b'x\0' + b'z' * 1022
    key = b'compact:capacity\0list'
    expiry = 4102444800123
    with server(binary, backend=backend) as source_port:
        with server(binary, backend=backend, replica_rpc_source=
                    '127.0.0.1:%d' % (source_port + 10000)) as replica_port:
            source, replica, watcher = (Client(source_port),
                                        Client(replica_port), Client(source_port))
            try:
                for client in (source, replica, watcher):
                    client.socket.settimeout(60)
                assert source.call('SELECT', 1) == b'OK'
                assert replica.call('SELECT', 1) == b'OK'
                for attempt in range(200):
                    token = str(attempt).encode()
                    assert source.call('SET', 'compact:ready', token) == b'OK'
                    if replica.call('GET', 'compact:ready') == token:
                        break
                    time.sleep(.01)
                else:
                    raise AssertionError('replica negotiation did not finish')
                assert source.call('SELECT', 0) == b'OK'
                assert replica.call('SELECT', 0) == b'OK'
                command = Client.encode(('LPUSH', key, value))
                for grouped in (False, True):
                    # Drain each chunk into the replica before sending more;
                    # this test checks atomic capacity, not queue saturation.
                    for start in range(0, 270000, 1000):
                        source.socket.sendall(command * 1000)
                        for i in range(1000):
                            assert source.read() == start + i + 1
                        wait_for(replica, ('LLEN', key), start + 1000)
                    assert source.call('PEXPIREAT', key, expiry) == 1
                    assert watcher.call('WATCH', key) == b'OK'
                    marker = b'exec' if grouped else b'ordinary'
                    if grouped:
                        assert source.call('MULTI') == b'OK'
                        for args in [('LTRIM', key, 264000, -1), ('SELECT', 1),
                                     ('SET', 'compact:done', marker), ('SELECT', 0)]:
                            assert source.call(*args) == b'QUEUED'
                        assert source.call('EXEC') == [b'OK'] * 4
                    else:
                        source.socket.sendall(Client.encode(
                            ('LTRIM', key, 264000, -1)) + Client.encode(
                            ('SET', 'compact:done', marker)))
                        assert source.read() == b'OK'
                        assert source.read() == b'OK'
                    assert source.call('LLEN', key) == 6000
                    assert watcher.call('MULTI') == b'OK'
                    assert watcher.call('LLEN', key) == b'QUEUED'
                    assert watcher.call('EXEC') is None
                    if grouped:
                        assert replica.call('SELECT', 1) == b'OK'
                    wait_for(replica, ('GET', 'compact:done'), marker)
                    if grouped:
                        assert replica.call('SELECT', 0) == b'OK'
                    assert replica.call('LLEN', key) == 6000
                    assert replica.call('PEXPIRETIME', key) == expiry
                    assert source.call('PEXPIRETIME', key) == expiry
                    assert replica.call('LRANGE', key, 0, -1) == [value] * 6000
                    assert source.call('LRANGE', key, 0, -1) == [value] * 6000
                    assert source.call('DEL', key) == 1
                    wait_for(replica, ('EXISTS', key), 0)
            finally:
                source.close()
                replica.close()
                watcher.close()
    print('Large binary LTRIM: ordinary/pipeline, cross-DB EXEC, WATCH, TTL, '
          'and live replica convergence passed')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--pkv', required=True)
    parser.add_argument('--backend', choices=('mdb', 'mdb_tbb'), default='mdb')
    args = parser.parse_args()
    run(args.pkv, args.backend)
