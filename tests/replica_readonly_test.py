#!/usr/bin/env python3
"""Replica client-write admission, CONFIG aliases and EXEC snapshots."""
import argparse
import contextlib
from integration_test import Client, ErrorReply, ports, server


def settings(client):
    values = client.call('CONFIG', 'GET', '*read-only')
    return dict(zip(values[::2], values[1::2]))


def exercise(clients, writers, endpoints):
    checks = 0

    def call(*args):
        nonlocal checks
        a, b = [client.call(*args) for client in clients]
        assert type(a) is type(b) and a == b, (args, a, b)
        checks += 1
        return a

    for client in clients:
        assert settings(client) == {b'replica-read-only': b'yes',
                                    b'slave-read-only': b'yes'}
    call('CONFIG', 'SET', 'replica-read-only', 'no')
    call('SET', 'seed', 'original')
    call('PFADD', 'sketch', 'a', 'b')
    call('GEOADD', 'geo', 1, 2, 'a')
    call('CONFIG', 'SET', 'replica-read-only', 'yes')
    for row in writers:
        args = [row[0]] + ['denied'] * (abs(row[1]) - 1)
        reply = call(*args)
        assert isinstance(reply, ErrorReply) and reply.startswith(b'READONLY')
    for args in [('GET', 'seed'), ('PFCOUNT', 'sketch'),
                 ('BITFIELD_RO', 'seed', 'GET', 'u8', 0),
                 ('GEOSEARCH', 'geo', 'FROMMEMBER', 'a', 'BYBOX', 0, 0, 'm'),
                 ('GEOPOS', 'geo', 'a'),
                 ('GEORADIUS_RO', 'geo', 1, 2, 1, 'm'),
                 ('GEORADIUSBYMEMBER_RO', 'geo', 'a', 0, 'm')]:
        call(*args)
    call('MULTI')
    call('SET', 'denied', 'value')
    call('PING')
    call('EXEC')
    call('CONFIG', 'SET', 'replica-read-only', 'no')
    call('WATCH', 'seed')
    call('MULTI')
    call('SET', 'changed-policy', 'value')
    peers = [Client(port) for port in endpoints]
    try:
        for peer in peers:
            assert peer.call('CONFIG', 'SET', 'replica-read-only',
                             'yes') == b'OK'
        reply = call('EXEC')
        assert isinstance(reply, ErrorReply) and reply.startswith(b'EXECABORT')
        call('GET', 'changed-policy')
        call('DISCARD')
        for peer in peers:
            peer.call('CONFIG', 'SET', 'replica-read-only', 'no')
            peer.call('SET', 'seed', 'changed')
            peer.call('CONFIG', 'SET', 'replica-read-only', 'yes')
        call('MULTI')
        call('PING')
        assert call('EXEC') == [b'PONG']
    finally:
        for peer in peers:
            peer.close()
    call('CONFIG', 'SET', 'replica-read-only', 'no')
    call('MULTI')
    call('CONFIG', 'SET', 'replica-read-only', 'yes')
    call('SET', 'inside', 'allowed')
    assert call('EXEC') == [b'OK', b'OK']
    assert call('GET', 'inside') == b'allowed'
    call('SET', 'outside', 'denied')
    call('MULTI')
    call('CONFIG', 'SET', 'replica-read-only', 'no')
    call('SET', 'still-readonly', 'denied')
    call('EXEC')
    for pairs in [('replica-read-only', 'YES'),
                  ('slave-read-only', 'no'),
                  ('replica-read-only', 'yes', 'slave-read-only', 'no'),
                  ('slave-read-only', 'yes', 'replica-read-only', 'no'),
                  ('replica-read-only', 'yes', 'replica-read-only', 'no'),
                  ('replica-read-only', 'bad'),
                  ('REPLICA-READ-ONLY', 'bad'),
                  ('SLAVE-READ-ONLY', 'bad'),
                  ('replica-read-only', 'no', 'acllog-max-len', 'bad'),
                  ('replica-read-only', b'yes\0tail')]:
        call('CONFIG', 'SET', *pairs)
        assert settings(clients[0]) == settings(clients[1])
    return checks


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--pkv', required=True)
    parser.add_argument('--redis', required=True)
    args = parser.parse_args()
    with contextlib.ExitStack() as stack:
        pair = [stack.enter_context(server(binary, redis=reference,
                replica_rpc_source=None if reference else
                '127.0.0.1:%d' % ports())) for binary, reference in
                [(args.pkv, False), (args.redis, True)]]
        clients = [Client(port) for port in pair]
        try:
            assert clients[1].call('REPLICAOF', '127.0.0.1', ports()) == b'OK'
            writers = [row for row in clients[0].call('COMMAND')
                       if b'write' in row[2]]
            assert writers
            checks = exercise(clients, writers, pair)
        finally:
            for client in clients:
                client.close()
    for binary, reference in [(args.pkv, False), (args.redis, True)]:
        with server(binary, redis=reference, replica_read_only=False,
                    replica_rpc_source=None if reference else
                    '127.0.0.1:%d' % ports()) as port:
            client = Client(port)
            try:
                if reference:
                    client.call('REPLICAOF', '127.0.0.1', ports())
                assert client.call('SET', 'startup', 'writable') == b'OK'
                assert settings(client)[b'replica-read-only'] == b'no'
            finally:
                client.close()
    print('Replica readonly differential assertions:', checks)
    print('Replica readonly write commands checked:', len(writers))
    print('Replica policy, aliases, startup and EXEC admission passed')


if __name__ == '__main__':
    main()
