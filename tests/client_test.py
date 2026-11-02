#!/usr/bin/env python3
"""CLIENT metadata, queue validation, RESET and redis-cli checks."""
import argparse
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import subprocess

from integration_test import Client, ErrorReply, server


def exercise(port, reference):
    a, b = Client(port), Client(reference)
    checks = 0

    def compare(*args):
        nonlocal checks
        actual, expected = a.call(*args), b.call(*args)
        assert actual == expected, (args[:3], actual, expected)
        checks += 1
        return actual

    try:
        for value in [b'', b'name', bytes(range(33, 127)), b'x' * 70000]:
            compare('CLIENT', 'SETNAME', value)
            compare('CLIENT', 'GETNAME')
        for value in [b' ', b'a b', b'a\tb', b'a\rb', b'a\nb',
                      b'\x01', b'\x7f', b'\x80', b'\xff']:
            compare('CLIENT', 'SETNAME', 'stable')
            compare('CLIENT', 'SETNAME', value)
            compare('CLIENT', 'GETNAME')
        for attribute in ['LIB-NAME', 'LIB-VER', 'lib-name', 'lIb-VeR']:
            for value in [b'', b'redis-client_1.2(suffix)',
                          bytes(range(33, 127)), b'x' * 70000,
                          b'bad name', b'\xff', b'new\nline']:
                compare('CLIENT', 'SETINFO', attribute, value)
        for attribute in ['unknown', 'Unknown', 'long-' + 'a' * 200,
                          b'a\r\n+FAKE']:
            compare('CLIENT', 'SETINFO', attribute, 'v')
            compare('PING')
        for args in [('CLIENT',), ('CLIENT', 'unknown'),
                     ('CLIENT', 'NoSuchSubcommand'),
                     ('CLIENT', 'x' * 200),
                     ('CLIENT', b'bad\r\n+FAKE'),
                     ('CLIENT', 'ID', 'extra'),
                     ('CLIENT', 'GETNAME', 'extra'),
                     ('CLIENT', 'SETNAME'),
                     ('CLIENT', 'SETNAME', 'v', 'extra'),
                     ('CLIENT', 'SETINFO'),
                     ('CLIENT', 'SETINFO', 'LIB-NAME'),
                     ('CLIENT', 'SETINFO', 'LIB-NAME', 'v', 'extra'),
                     ('CLIENT', 'HELP', 'extra'), ('RESET', 'extra')]:
            compare(*args)
            compare('PING')
        for c in [a, b]:
            request = [('CLIENT', 'SETINFO', 'LIB-NAME', 'sdk'),
                       ('CLIENT', 'SETINFO', 'LIB-VER', '1.0'),
                       ('CLIENT', 'SETNAME', 'pipeline'),
                       ('CLIENT', 'GETNAME')]
            c.socket.sendall(b''.join(c.encode(args) for args in request))
            assert [c.read() for _ in request] == \
                [b'OK', b'OK', b'OK', b'pipeline']
        compare('SET', 'reset-key', 'db0')
        compare('SELECT', 1)
        compare('SET', 'reset-key', 'db1')
        ids = [c.call('CLIENT', 'ID') for c in [a, b]]
        assert all(isinstance(i, int) and i > 0 for i in ids)
        compare('WATCH', 'reset-key')
        compare('MULTI')
        compare('SET', 'reset-key', 'queued')
        compare('CLIENT', 'SETNAME', 'queued-name')
        compare('RESET')
        compare('CLIENT', 'GETNAME')
        compare('GET', 'reset-key')
        assert [c.call('CLIENT', 'ID') for c in [a, b]] == ids
        compare('EXEC')
        compare('SELECT', 1)
        compare('GET', 'reset-key')
        compare('RESET')
        compare('MULTI')
        compare('CLIENT', 'SETNAME')
        compare('EXEC')
        compare('MULTI')
        compare('CLIENT', 'unknown')
        compare('RESET')
        compare('EXEC')
        compare('MULTI')
        compare('CLIENT', 'SETNAME', 'valid')
        compare('CLIENT', 'SETNAME', 'bad name')
        compare('CLIENT', 'SETINFO', 'unknown', 'x')
        compare('CLIENT', 'GETNAME')
        compare('SET', 'client-txn', 'value')
        compare('EXEC')
        compare('GET', 'client-txn')
        compare('CLIENT', 'GETNAME')
        for command in [('CLIENT', 'ID'), ('CLIENT', 'SETNAME', 'a'),
                        ('CLIENT', 'SETINFO', 'LIB-NAME', 'x'), ('RESET',)]:
            compare('COMMAND', 'GETKEYS', *command)
            compare('COMMAND', 'GETKEYSANDFLAGS', *command)
        for name in ['client|id', 'client|setname', 'client|setinfo', 'reset']:
            compare('COMMAND', 'INFO', name)
        names = a.call('COMMAND', 'LIST', 'FILTERBY', 'PATTERN', 'client|*')
        assert set(names) == {b'client|id', b'client|getname',
                              b'client|setname', b'client|setinfo',
                              b'client|help', b'client|kill', b'client|info',
                              b'client|list', b'client|unblock'}
        compare('COMMAND', 'INFO', 'client|kill')
        assert isinstance(a.call('CLIENT', 'HELP'), list)
        strict(a)
        watch_reset(port)
        watch_reset(reference)
        return checks
    finally:
        a.close()
        b.close()


def strict(c):
    assert c.call('CLIENT', 'SETNAME', 'stable') == b'OK'
    for value in [b'prefix\0suffix', b'\0', b'prefix\0\r\n+FAKE']:
        assert c.call('CLIENT', 'SETNAME', value) == ErrorReply(
            b'ERR Client names cannot contain spaces, newlines or special '
            b'characters.')
        assert c.call('CLIENT', 'GETNAME') == b'stable'
        for attribute in ['LIB-NAME', 'LIB-VER']:
            assert isinstance(c.call('CLIENT', 'SETINFO', attribute, value),
                              ErrorReply)
            assert c.call('PING') == b'PONG'
    for command in [('CLIENT', b'ID\0extra'),
                    ('CLIENT', 'SETINFO', b'LIB-NAME\0extra', 'v')]:
        assert isinstance(c.call(*command), ErrorReply)
        assert c.call('PING') == b'PONG'


def watch_reset(port):
    a, b = Client(port), Client(port)
    try:
        assert a.call('SET', 'reset-watch', 'old') == b'OK'
        assert a.call('WATCH', 'reset-watch') == b'OK'
        assert a.call('RESET') == b'RESET'
        assert b.call('SET', 'reset-watch', 'new') == b'OK'
        assert a.call('MULTI') == b'OK'
        assert a.call('GET', 'reset-watch') == b'QUEUED'
        assert a.call('EXEC') == [b'new']
        named = [('CLIENT', 'SETNAME', 'kept-reply'),
                 ('CLIENT', 'GETNAME'), ('RESET',),
                 ('CLIENT', 'GETNAME'), ('PING',)]
        a.socket.sendall(b''.join(a.encode(args) for args in named))
        assert [a.read() for _ in named] == \
            [b'OK', b'kept-reply', b'RESET', None, b'PONG']
        request = [('MULTI',), ('SET', 'never', 'committed'), ('RESET',),
                   ('CLIENT', 'GETNAME'), ('GET', 'never')]
        a.socket.sendall(b''.join(a.encode(args) for args in request))
        assert [a.read() for _ in request] == \
            [b'OK', b'QUEUED', b'RESET', None, None]
    finally:
        a.close()
        b.close()


def connections(port):
    def worker(index):
        c = Client(port)
        try:
            identity = c.call('CLIENT', 'ID')
            name = 'connection-%d' % index
            assert c.call('CLIENT', 'SETNAME', name) == b'OK'
            assert c.call('CLIENT', 'GETNAME') == name.encode()
            assert c.call('RESET') == b'RESET'
            assert c.call('CLIENT', 'ID') == identity
            assert c.call('CLIENT', 'GETNAME') is None
            return identity
        finally:
            c.close()

    with ThreadPoolExecutor(max_workers=8) as pool:
        identities = list(pool.map(worker, range(32)))
    assert len(set(identities)) == len(identities) and min(identities) > 0
    c = Client(port)
    try:
        assert c.call('CLIENT', 'ID') > max(identities)
    finally:
        c.close()


def cli(binary, port):
    program = Path(binary).with_name('redis-cli')
    if not program.is_file():
        print('redis-cli integration skipped: binary unavailable')
        return
    request = b'CLIENT SETNAME cli-client\nCLIENT GETNAME\n'
    request += b'CLIENT SETINFO LIB-NAME redis-cli\nRESET\nCLIENT GETNAME\n'
    result = subprocess.run([str(program), '-h', '127.0.0.1', '-p', str(port),
                             '--raw'], input=request, capture_output=True,
                            check=True, timeout=10)
    assert result.stdout.splitlines() == \
        [b'OK', b'cli-client', b'OK', b'RESET', b''], result
    print('redis-cli connection setup and RESET passed')


def cluster(binary):
    with server(binary, cluster=True) as port:
        c = Client(port)
        try:
            assert c.call('CLIENT', 'SETNAME', '{unserved}name') == b'OK'
            assert c.call('CLIENT', 'GETNAME') == b'{unserved}name'
            assert c.call('MULTI') == b'OK'
            assert c.call('CLIENT', 'SETNAME', '{other}name') == b'QUEUED'
            assert c.call('PING') == b'QUEUED'
            assert c.call('EXEC') == [b'OK', b'PONG']
            assert c.call('RESET') == b'RESET'
            key = '{served}key'
            slot = c.call('CLUSTER', 'KEYSLOT', key)
            assert c.call('CLUSTER', 'ADDSLOTS', slot) == b'OK'
            assert c.call('MULTI') == b'OK'
            assert c.call('CLIENT', 'SETNAME', '{elsewhere}name') == b'QUEUED'
            assert c.call('SET', key, 'value') == b'QUEUED'
            assert c.call('EXEC') == [b'OK', b'OK']
            assert c.call('CLIENT', 'GETNAME') == b'{elsewhere}name'
        finally:
            c.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--pkv', required=True)
    parser.add_argument('--redis', required=True)
    args = parser.parse_args()
    with server(args.redis, redis=True) as ref, server(args.pkv) as port:
        checks = exercise(port, ref)
        connections(port)
        connections(ref)
        cli(args.redis, port)
    cluster(args.pkv)
    print('CLIENT/RESET differential assertions:', checks)


if __name__ == '__main__':
    main()
