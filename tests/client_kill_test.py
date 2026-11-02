#!/usr/bin/env python3
"""CLIENT KILL grammar, filters, reply ordering and cross-thread shutdown."""
import argparse
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import subprocess
import time

from integration_test import Client, ErrorReply, server
from resp3_client import RespClient


def closed(client):
    client.socket.settimeout(3)
    assert client.stream.read(1) == b'', 'connection remained open'


def validation(port, reference):
    a, b = Client(port), Client(reference)
    checks = 0
    try:
        requests = [('CLIENT', 'KILL'),
                    ('CLIENT', 'KILL', 'not-an-address'),
                    ('CLIENT', 'KILL', 'ID', '9223372036854775807'),
                    ('CLIENT', 'KILL', 'TYPE', 'pubsub')]
        for value in ['', '0', '-1', '+1', '01', '-0', '1 ',
                      '9223372036854775808', b'1\0x']:
            requests += [('CLIENT', 'KILL', 'ID', value)]
        for value in ['', '0', '-1', '01', '+1', 'no',
                      '9223372036854775808', b'1\0x']:
            requests += [('CLIENT', 'KILL', 'MAXAGE', value)]
        for kind in ['master', 'slave', 'replica', 'pubsub', 'MASTER']:
            requests += [('CLIENT', 'KILL', 'TYPE', kind)]
        requests += [('CLIENT', 'KILL', 'TYPE', 'no-such-type'),
                     ('CLIENT', 'KILL', 'USER', 'Default'),
                     ('CLIENT', 'KILL', 'USER', 'no-such-user'),
                     ('CLIENT', 'KILL', 'SKIPME', 'maybe'),
                     ('CLIENT', 'KILL', 'ID', '1', 'ID'),
                     ('CLIENT', 'KILL', 'UNKNOWN', 'value')]
        for args in requests:
            actual, expected = a.call(*args), b.call(*args)
            assert type(actual) is type(expected) and actual == expected, \
                (args, actual, expected)
            checks += 1
            assert a.call('PING') == b.call('PING') == b'PONG'
        for client in [a, b]:
            own = client.call('CLIENT', 'ID')
            assert client.call('CLIENT', 'KILL', 'ID', own) == 0
            assert client.call('CLIENT', 'KILL', 'ID', own,
                               'SKIPME', 'no', 'SKIPME', 'yes') == 0
            assert client.call('PING') == b'PONG'
        return checks
    finally:
        a.close()
        b.close()


def filters(port):
    admin = Client(port)
    try:
        for name in ['id', 'addr', 'laddr', 'legacy', 'duplicates', 'age']:
            victim = Client(port)
            try:
                identity = victim.call('CLIENT', 'ID')
                address = '%s:%s' % victim.socket.getsockname()
                local = '%s:%s' % victim.socket.getpeername()
                base = ['CLIENT', 'KILL', 'ID', identity]
                assert isinstance(admin.call(*base, 'UNKNOWN', 'x'),
                                  ErrorReply)
                assert victim.call('PING') == b'PONG'
                assert admin.call(*base, 'ADDR', 'wrong') == 0
                assert admin.call(*base, 'LADDR', 'wrong') == 0
                assert admin.call(*base, 'TYPE', 'pubsub') == 0
                assert admin.call(*base, 'MAXAGE', 3600) == 0
                assert victim.call('PING') == b'PONG'
                if name == 'legacy':
                    assert admin.call('CLIENT', 'KILL', address) == b'OK'
                elif name == 'duplicates':
                    assert admin.call('CLIENT', 'KILL', 'ID',
                                      9223372036854775807, 'ID', identity,
                                      'TYPE', 'pubsub', 'TYPE', 'NORMAL') == 1
                elif name == 'age':
                    time.sleep(1.15)
                    assert admin.call(*base, 'MAXAGE', 1) == 1
                elif name == 'id':
                    assert admin.call(*base) == 1
                else:
                    assert admin.call(*base, 'ADDR', address, 'LADDR', local,
                                      'TYPE', 'normal', 'USER', 'default') == 1
                closed(victim)
                assert admin.call(*base) == 0
            finally:
                victim.close()
    finally:
        admin.close()


def transactions(port, reference=False):
    admin, observer = Client(port), Client(port)
    try:
        victim = Client(port)
        identity = victim.call('CLIENT', 'ID')
        try:
            assert admin.call('MULTI') == b'OK'
            assert admin.call('CLIENT', 'KILL', 'ID', identity) == b'QUEUED'
            assert victim.call('PING') == b'PONG'
            assert admin.call('DISCARD') == b'OK'
            assert victim.call('PING') == b'PONG'
            assert admin.call('MULTI') == b'OK'
            assert admin.call('CLIENT', 'KILL', 'ID', identity) == b'QUEUED'
            assert victim.call('PING') == b'PONG'
            assert admin.call('EXEC') == [1]
            closed(victim)
        finally:
            victim.close()
        for legacy in [False, True]:
            own = Client(port)
            try:
                kill = ['CLIENT', 'KILL']
                if legacy:
                    kill += ['%s:%s' % own.socket.getsockname()]
                else:
                    kill += ['ID', own.call('CLIENT', 'ID'), 'SKIPME', 'NO']
                key = 'kill-pipeline-' + str(legacy)
                own.socket.sendall(own.encode(kill) +
                                   own.encode(['SET', key, 'bad']))
                assert own.read() == (b'OK' if legacy else 1)
                closed(own)
                assert observer.call('GET', key) is None
            finally:
                own.close()
        own = Client(port)
        try:
            identity = own.call('CLIENT', 'ID')
            assert own.call('MULTI') == b'OK'
            assert own.call('CLIENT', 'KILL', 'ID', identity,
                            'SKIPME', 'NO') == b'QUEUED'
            assert own.call('SET', 'kill-exec', 'committed') == b'QUEUED'
            if reference:
                own.socket.sendall(own.encode(['EXEC']))
                assert own.stream.read() == b'*2\r\n:1\r\n'
            else:
                assert own.call('EXEC') == [1, b'OK']
            closed(own)
            assert observer.call('GET', 'kill-exec') == b'committed'
        finally:
            own.close()
        assert admin.call('MULTI') == b'OK'
        assert isinstance(admin.call('CLIENT', 'KILL'), ErrorReply)
        assert admin.call('EXEC').startswith(b'EXECABORT')
        assert admin.call('MULTI') == b'OK'
        assert admin.call('CLIENT', 'KILL', 'ID', 0) == b'QUEUED'
        assert admin.call('SET', 'kill-runtime-error', 'ok') == b'QUEUED'
        result = admin.call('EXEC')
        assert isinstance(result[0], ErrorReply) and result[1] == b'OK'
    finally:
        admin.close()
        observer.close()


def races(port):
    victim = Client(port)
    identity = victim.call('CLIENT', 'ID')

    def killer(_):
        c = Client(port)
        try:
            return c.call('CLIENT', 'KILL', 'ID', identity)
        finally:
            c.close()

    try:
        with ThreadPoolExecutor(max_workers=8) as pool:
            results = list(pool.map(killer, range(32)))
        assert set(results) <= {0, 1} and sum(results) == 1, results
        closed(victim)
    finally:
        victim.close()


def resp3(port):
    c = RespClient(port)
    try:
        assert c.call('HELLO', 3).kind == b'%'
        identity = c.call('CLIENT', 'ID').value
        assert c.call('CLIENT', 'KILL', 'ID', identity).value == 0
        reply = c.call('CLIENT', 'KILL', 'ID', identity, 'SKIPME', 'NO')
        assert reply.kind == b':' and reply.value == 1
        closed(c)
    finally:
        c.close()


def protected(binary, reference):
    for path, redis in [(reference, True), (binary, False)]:
        with server(path, redis=redis, password='kill-secret') as port:
            admin, victim = Client(port), Client(port)
            try:
                assert admin.call('AUTH', 'kill-secret') == b'OK'
                assert victim.call('AUTH', 'kill-secret') == b'OK'
                identity = victim.call('CLIENT', 'ID')
                assert victim.call('RESET') == b'RESET'
                result = victim.call('CLIENT', 'KILL', 'ID', identity,
                                     'SKIPME', 'NO')
                assert isinstance(result, ErrorReply) and \
                    result.startswith(b'NOAUTH')
                assert admin.call('CLIENT', 'KILL', 'ID', identity,
                                  'USER', 'default') == 1
                closed(victim)
            finally:
                admin.close()
                victim.close()


def cli(reference, port):
    program = Path(reference).with_name('redis-cli')
    victim = Client(port)
    try:
        identity = victim.call('CLIENT', 'ID')
        result = subprocess.run([str(program), '-p', str(port),
                                 'CLIENT', 'KILL', 'ID', str(identity)],
                                capture_output=True, timeout=10, check=True)
        assert result.stdout == b'1\n', result
        closed(victim)
    finally:
        victim.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--pkv', required=True)
    parser.add_argument('--redis', required=True)
    args = parser.parse_args()
    with server(args.redis, redis=True) as ref, server(args.pkv) as port:
        checks = validation(port, ref)
        for target in [ref, port]:
            filters(target)
            transactions(target, target == ref)
            races(target)
            resp3(target)
            cli(args.redis, target)
    protected(args.pkv, args.redis)
    with server(args.pkv, cluster=True) as port:
        c = Client(port)
        try:
            identity = c.call('CLIENT', 'ID')
            assert c.call('CLIENT', 'KILL', 'ID', identity,
                          'SKIPME', 'NO') == 1
            closed(c)
        finally:
            c.close()
    print('CLIENT KILL common differential assertions:', checks)
    print('CLIENT KILL filters, pipelines, EXEC, races, AUTH and CLI passed')


if __name__ == '__main__':
    main()
