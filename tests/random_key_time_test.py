#!/usr/bin/env python3
"""RANDOMKEY populations and typed TIME, transactions, ACL and lifecycle."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import socket
import time

from integration_test import Client, server
from resp3_client import RespClient, canonical

CHECKS = 0


def same(a, b, *args):
    global CHECKS
    x, y = a.call(*args), b.call(*args)
    assert canonical(x) == canonical(y), (args, x, y)
    CHECKS += 1
    return x


def clock(client):
    global CHECKS
    before = time.time_ns() // 1000
    reply = client.call('TIME')
    after = time.time_ns() // 1000
    assert reply.kind == b'*' and len(reply.value) == 2, reply
    seconds, micros = reply.value
    assert seconds.kind == micros.kind == b'$', reply
    assert seconds.value == str(int(seconds.value)).encode()
    assert micros.value == str(int(micros.value)).encode()
    assert 0 <= int(micros.value) < 1000000, reply
    observed = int(seconds.value) * 1000000 + int(micros.value)
    # The reference caches wall time briefly. Do not require strict equality
    # with the separate Python process's two clock reads.
    assert before - 100000 <= observed <= after + 100000, reply
    CHECKS += 1
    return reply


def basic(pkv, reference, protocol):
    with server(pkv, active_expire=False) as p, \
            server(reference, redis=True) as r:
        a, b = RespClient(p), RespClient(r)
        try:
            if protocol == 3:
                for client in [a, b]:
                    assert client.call('HELLO', 3).kind == b'%'
            for args in [('RANDOMKEY',), ('RANDOMKEY', 'extra'),
                         ('TIME', 'extra'), ('RANDOMKEY', 'a', 'b')]:
                same(a, b, *args)
            for client in [a, b]:
                clock(client)
            for name in ['randomkey', 'time']:
                same(a, b, 'COMMAND', 'INFO', name)
                same(a, b, 'COMMAND', 'GETKEYS', name)
            binary = b'\0\xff\r\nkey'
            seeds = [('SET', '', 'empty-key'), ('SET', binary, b'\0\xff'),
                     ('LPUSH', 'list', 'a', 'b'),
                     ('HSET', 'hash', 'a', 'b'), ('SADD', 'set', 'a', 'b'),
                     ('ZADD', 'zset', 1, 'a', 2, 'b')]
            for args in seeds:
                key = args[1]
                same(a, b, *args)
                for _ in range(3):
                    reply = same(a, b, 'RANDOMKEY')
                    assert reply.kind == b'$' and reply.value == (
                        key if isinstance(key, bytes) else key.encode())
                same(a, b, 'WATCH', key)
                same(a, b, 'RANDOMKEY')
                same(a, b, 'MULTI')
                same(a, b, 'PING')
                same(a, b, 'EXEC')
                same(a, b, 'DEL', key)
            same(a, b, 'SET', 'large', b'v' * 70000)
            same(a, b, 'PEXPIRE', 'large', 60000)
            same(a, b, 'RANDOMKEY')
            for client in [a, b]:
                assert client.call('PTTL', 'large').value > 0
            same(a, b, 'GET', 'large')
            same(a, b, 'DEL', 'large')
            for i in range(80):
                same(a, b, 'SET', 'expired:' + str(i), 'x', 'PX', 20)
            time.sleep(.08)
            same(a, b, 'RANDOMKEY')
            same(a, b, 'DBSIZE')
            names = {b'key:' + str(i).encode() for i in range(128)}
            for key in names:
                same(a, b, 'SET', key, 'v')
            for _ in range(200):
                for client in [a, b]:
                    reply = client.call('RANDOMKEY')
                    assert reply.kind == b'$' and reply.value in names
            same(a, b, 'SELECT', 2)
            same(a, b, 'RANDOMKEY')
            same(a, b, 'SET', 'tx:old', 'old')
            same(a, b, 'MULTI')
            for args in [('DEL', 'tx:old'), ('SET', 'tx:new', 'new'),
                         ('SET', 'tx:new', 'overwritten'), ('TIME',),
                         ('RANDOMKEY',), ('PING',), ('TIME',)]:
                same(a, b, *args)
            for client in [a, b]:
                result = client.call('EXEC')
                assert result.kind == b'*' and len(result.value) == 7
                assert result.value[4].kind == b'$'
                assert result.value[4].value == b'tx:new'
                assert canonical(result.value[3]) == canonical(result.value[6])
            same(a, b, 'SELECT', 3)
            for _ in range(3):
                same(a, b, 'RANDOMKEY')
            same(a, b, 'MULTI')
            for args in [('SET', 'only', 'new'), ('RANDOMKEY',),
                         ('DEL', 'only'), ('RANDOMKEY',)]:
                same(a, b, *args)
            same(a, b, 'EXEC')
        finally:
            a.close()
            b.close()


def policies(pkv, reference):
    with server(pkv) as p, server(reference, redis=True) as r:
        a, b = RespClient(p), RespClient(r)
        c, d = RespClient(p), RespClient(r)
        try:
            same(a, b, 'SET', 'secret', 'v')
            same(a, b, 'ACL', 'SETUSER', 'sample', 'on', 'nopass',
                 '+time', '+randomkey', '+auth', '+hello')
            same(c, d, 'AUTH', 'sample', 'unused')
            same(c, d, 'RANDOMKEY')
            clock(c)
            clock(d)
            same(c, d, 'GET', 'secret')
            same(a, b, 'ACL', 'SETUSER', 'sample', '-randomkey')
            same(c, d, 'RANDOMKEY')
            for client in [a, b]:
                assert client.call('SUBSCRIBE', 'chan').kind == b'*'
            same(a, b, 'TIME')
            same(a, b, 'RANDOMKEY')
            same(a, b, 'RESET')
            for client in [a, b]:
                assert client.call('HELLO', 3).kind == b'%'
                assert client.call('SUBSCRIBE', 'chan').kind == b'>'
                clock(client)
            same(a, b, 'RANDOMKEY')
            same(a, b, 'UNSUBSCRIBE')
        finally:
            for client in [a, b, c, d]:
                client.close()


def cluster(pkv, reference):
    with server(pkv, cluster=True) as p, \
            server(reference, redis=True, cluster=True) as r:
        a, b = RespClient(p), RespClient(r)
        try:
            same(a, b, 'RANDOMKEY')
            clock(a)
            clock(b)
        finally:
            a.close()
            b.close()


def concurrent(pkv):
    with server(pkv) as port:
        admin = Client(port)
        payload = b'x' * 70000
        def writer(index):
            client = Client(port)
            try:
                key = 'writer:' + str(index)
                for _ in range(40):
                    assert client.call('SET', key, payload) == b'OK'
                    assert client.call('DEL', key) == 1
            finally:
                client.close()
        try:
            with ThreadPoolExecutor(max_workers=4) as pool:
                jobs = [pool.submit(writer, i) for i in range(4)]
                for _ in range(400):
                    key = admin.call('RANDOMKEY')
                    assert key is None or key in {
                        b'writer:0', b'writer:1', b'writer:2', b'writer:3'}
                for job in jobs:
                    job.result()
            assert admin.call('RANDOMKEY') is None
        finally:
            admin.close()


def replica(pkv, reference):
    with socket.socket() as unavailable:
        unavailable.bind(('127.0.0.1', 0))
        source = unavailable.getsockname()[1]
        with server(pkv, replica_rpc_source='127.0.0.1:' + str(source)) as p, \
                server(reference, redis=True) as r:
            a, b = RespClient(p), RespClient(r)
            try:
                assert b.call('REPLICAOF', '127.0.0.1', source).value == b'OK'
                clock(a)
                clock(b)
                same(a, b, 'RANDOMKEY')
                same(a, b, 'MULTI')
                same(a, b, 'RANDOMKEY')
                for client in [a, b]:
                    assert client.call('TIME').value == b'QUEUED'
                    result = client.call('EXEC')
                    assert result.kind == b'*' and len(result.value) == 2
                    assert result.value[1].kind == b'*'
            finally:
                a.close()
                b.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--pkv', required=True)
    parser.add_argument('--redis', required=True)
    args = parser.parse_args()
    for protocol in [2, 3]:
        basic(args.pkv, args.redis, protocol)
    policies(args.pkv, args.redis)
    cluster(args.pkv, args.redis)
    replica(args.pkv, args.redis)
    concurrent(args.pkv)
    print('Random key/time behavioral checks:', CHECKS)
    print('RANDOMKEY/TIME protocols, expiry, ACL, EXEC and races passed')


if __name__ == '__main__':
    main()
