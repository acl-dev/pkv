#!/usr/bin/env python3
"""Default-user authentication and protected replication stream checks."""
import argparse
import os
from pathlib import Path
import subprocess
import socket
import time

from integration_test import Client, ErrorReply, server
from resp3_client import RespClient, Reply, mapping

PASSWORD = 'correct-horse-0123456789'
NOAUTH = ErrorReply(b'NOAUTH Authentication required.')
EXECNOAUTH = ErrorReply(b'EXECABORT Transaction discarded because of: '
                       b'NOAUTH Authentication required.')


def compare_clients(port, reference, password):
    a, b = Client(port), Client(reference)
    checks = 0

    def compare(*args):
        nonlocal checks
        x, y = a.call(*args), b.call(*args)
        assert x == y, (args[:2], x, y)
        checks += 1
        return x

    try:
        for command in [('PING',), ('GET', 'key'), ('SET', 'key', 'x'),
                        ('MGET', 'key', 'x'), ('HGETALL', 'hash'),
                        ('LPUSH', 'list', 'x'), ('SADD', 'set', 'x'),
                        ('ZADD', 'zset', 1, 'x'), ('SELECT', 1),
                        ('DBSIZE',), ('KEYS', '*'), ('SCAN', 0),
                        ('WATCH', 'key'), ('MULTI',), ('EXEC',), ('DISCARD',),
                        ('UNWATCH',), ('COMMAND', 'INFO', 'auth'),
                        ('CONFIG', 'GET', 'save'), ('CLIENT', 'ID'),
                        ('CLIENT', 'SETNAME', 'name'),
                        ('CLIENT', 'SETINFO', 'LIB-NAME', 'lib')]:
            assert compare(*command) == (EXECNOAUTH if command[0] ==
                                         'EXEC' else NOAUTH)
        for args in [(), ('wrong',), ('default', 'wrong'),
                     ('missing', password), ('Default', password),
                     (b'default\0x', password),
                     ('default', password + '\0'),
                     ('default', password, 'extra')]:
            compare('AUTH', *args)
        for candidate in [password[1:], password + 'x',
                          'x' + password[1:], password[:-1] + 'x',
                          password[:6] + 'x' + password[7:]]:
            compare('AUTH', candidate)
            assert compare('PING') == NOAUTH
        assert compare('AUTH', password) == b'OK'
        compare('PING')
        compare('CLIENT', 'SETNAME', 'retained')
        compare('SELECT', 1)
        compare('SET', 'key', 'db1')
        compare('AUTH', 'wrong')
        compare('GET', 'key')
        compare('CLIENT', 'GETNAME')
        compare('AUTH', 'default', password)
        compare('GET', 'key')
        compare('MULTI')
        compare('AUTH', 'default', password, 'extra')
        compare('AUTH', password)
        compare('SET', 'queued', 'written')
        compare('GET', 'queued')
        compare('EXEC')
        compare('RESET')
        assert compare('GET', 'key') == NOAUTH
        compare('AUTH', password)
        compare('GET', 'key')
        compare('CLIENT', 'GETNAME')
        compare('SELECT', 1)
        compare('GET', 'queued')
        compare('MULTI')
        compare('SET', 'discarded', 'never')
        compare('RESET')
        assert compare('EXEC') == EXECNOAUTH
        compare('AUTH', password)
        compare('EXEC')
        compare('SELECT', 1)
        compare('GET', 'discarded')
        compare('COMMAND', 'INFO', 'auth')
        return checks
    finally:
        a.close()
        b.close()


def handshake(port, reference, password):
    for selected in [port, reference]:
        c = RespClient(selected)
        try:
            for args in [(), (3,), (3, 'SETNAME', 'uncommitted')]:
                assert c.call('HELLO', *args).kind == b'-'
            for args in [(3, 'AUTH', 'default', 'wrong'),
                         (3, 'AUTH', 'missing', password),
                         (3, 'AUTH', 'default', password,
                          'SETNAME', 'bad name')]:
                assert c.call('HELLO', *args).kind == b'-'
                assert c.call('PING') == Reply(b'-', NOAUTH)
            info = mapping(c.call('HELLO', 3, 'AUTH', 'default', password,
                                  'SETNAME', 'authed'))
            assert info[b'proto'] == Reply(b':', 3)
            assert c.call('CLIENT', 'GETNAME') == Reply(b'$', b'authed')
            identity = c.call('CLIENT', 'ID')
            assert c.call('AUTH', 'wrong').kind == b'-'
            assert c.call('GET', 'none') == Reply(b'_', None)
            assert c.call('RESET') == Reply(b'+', b'RESET')
            assert c.call('GET', 'none') == Reply(b'-', NOAUTH)
            assert c.call('AUTH', password) == Reply(b'+', b'OK')
            assert c.call('CLIENT', 'ID') == identity
            assert c.call('GET', 'none') == Reply(b'$', None)
        finally:
            c.close()


def watching(port, reference, password):
    for selected in [port, reference]:
        a, b = Client(selected), Client(selected)
        try:
            assert a.call('AUTH', password) == b'OK'
            assert b.call('AUTH', 'default', password) == b'OK'
            for wrong in [False, True]:
                assert a.call('SET', 'watch-auth', 'old') == b'OK'
                assert a.call('WATCH', 'watch-auth') == b'OK'
                result = a.call('AUTH', 'wrong' if wrong else password)
                assert isinstance(result, ErrorReply) if wrong else \
                    result == b'OK'
                assert b.call('SET', 'watch-auth', 'new') == b'OK'
                assert a.call('MULTI') == b'OK'
                assert a.call('GET', 'watch-auth') == b'QUEUED'
                assert a.call('EXEC') is None
        finally:
            a.close()
            b.close()


def open_server(port, reference):
    a, b = Client(port), Client(reference)
    try:
        for args in [('AUTH',), ('AUTH', 'password'),
                     ('AUTH', 'default', 'anything'),
                     ('AUTH', 'default', b'anything\0'),
                     ('AUTH', 'missing', 'anything'),
                     ('AUTH', 'default', 'anything', 'extra'), ('PING',)]:
            assert a.call(*args) == b.call(*args), args
    finally:
        a.close()
        b.close()


def strict_framing(port, password):
    c = Client(port)
    wrong = ErrorReply(b'WRONGPASS invalid username-password pair or user '
                       b'is disabled.')
    try:
        assert c.call('AUTH', password) == b'OK'
        for args in [('AUTH', 'wrong'),
                     ('HELLO', 3, 'AUTH', 'default', 'wrong')]:
            pipeline = [('PING',), args, ('PING',)]
            c.socket.sendall(b''.join(c.encode(v) for v in pipeline))
            assert [c.read() for _ in pipeline] == [b'PONG', wrong, b'PONG']
            assert c.call('MULTI') == b'OK'
            assert c.call('SET', 'strict-auth', 'written') == b'QUEUED'
            assert c.call(*args) == b'QUEUED'
            assert c.call('GET', 'strict-auth') == b'QUEUED'
            c.socket.sendall(c.encode(['EXEC']) + c.encode(['PING']))
            assert c.read() == [b'OK', wrong, b'written']
            assert c.read() == b'PONG'
            assert c.call('GET', 'strict-absent') is None
    finally:
        c.close()


def rpc_socket(port):
    deadline = time.monotonic() + 3
    while True:
        try:
            return socket.create_connection(
                ('127.0.0.1', port + 10000), 3)
        except ConnectionRefusedError:
            assert time.monotonic() < deadline
            time.sleep(0.01)


def rpc(binary):
    with server(binary, password=PASSWORD) as port:
        c = Client(port)
        try:
            assert c.call('AUTH', PASSWORD) == b'OK'
            for request in [b'PKV-REPL 3\r\n', b'PKV-AUTH -1\r\n',
                            b'PKV-AUTH 999999999999999999\r\n',
                            b'PKV-AUTH 01\r\n',
                            b'PKV-AUTH 1\r\nx\r\n',
                            b'PKV-AUTH %d\r\n' % len(PASSWORD) +
                            b'x' * len(PASSWORD) + b'\r\n']:
                s = rpc_socket(port)
                try:
                    s.sendall(request)
                    assert c.call('SET', 'rpc-private', 'secret') == b'OK'
                    try:
                        assert s.recv(32) == b'', 'unauthenticated RPC data'
                    except ConnectionResetError:
                        pass
                finally:
                    s.close()
            idle = rpc_socket(port)
            try:
                assert c.call('SET', 'rpc-private', 'idle-secret') == b'OK'
                idle.settimeout(0.15)
                try:
                    assert idle.recv(1) == b''
                except socket.timeout:
                    pass
            finally:
                idle.close()
            slow = rpc_socket(port)
            try:
                slow.sendall(b'PKV-AUTH %d\r\n' % len(PASSWORD))
                started = time.monotonic()
                closed = False
                slow.settimeout(0.1)
                while time.monotonic() - started < 7:
                    try:
                        slow.sendall(b'x')
                        if slow.recv(1) == b'':
                            closed = True
                            break
                    except (BrokenPipeError, ConnectionResetError):
                        closed = True
                        break
                    except socket.timeout:
                        pass
                    assert c.call('PING') == b'PONG'
                    time.sleep(0.3)
                assert closed, 'trickled RPC authentication never expired'
            finally:
                slow.close()
            for version in [1, 2, 3]:
                s = rpc_socket(port)
                try:
                    frame = b'PKV-AUTH %d\r\n' % len(PASSWORD)
                    frame += PASSWORD.encode() + b'\r\n'
                    frame += b'PKV-REPL %d\r\n' % version
                    s.sendall(frame)
                    stream = s.makefile('rb')
                    assert stream.read(5) == b'+OK\r\n'
                    ready = stream.read(20)
                    assert len(ready) == 20 and ready[:3] == b'PKV', ready
                    stream.close()
                finally:
                    s.close()
        finally:
            c.close()


def replication(binary, password=PASSWORD, test_wrong=True):
    with server(binary, password=password) as source:
        address = '127.0.0.1:%d' % (source + 10000)
        credentials = ['wrong-password', password] if test_wrong else [password]
        for credential in credentials:
            with server(binary, password='receiver-password',
                        replica_rpc_source=address,
                        masterauth=credential) as target:
                a, b = Client(source), Client(target)
                try:
                    assert a.call('AUTH', password) == b'OK'
                    assert b.call('AUTH', 'receiver-password') == b'OK'
                    deadline = time.monotonic() + (
                        6 if credential == password else 1.3)
                    while time.monotonic() < deadline:
                        assert a.call('SET', 'auth-marker', credential) == b'OK'
                        value = b.call('GET', 'auth-marker')
                        if credential == password and value == \
                                password.encode():
                            break
                        if credential != password:
                            assert value is None, 'wrong credential replicated'
                        time.sleep(0.02)
                    if credential == password:
                        assert b.call('GET', 'auth-marker') == \
                            password.encode()
                        assert a.call('MULTI') == b'OK'
                        assert a.call('HSET', 'auth-hash', 'f',
                                      b'x' * 70000) == b'QUEUED'
                        assert a.call('SET', 'auth-value', 'committed') == \
                            b'QUEUED'
                        assert a.call('EXEC') == [1, b'OK']
                        deadline = time.monotonic() + 6
                        while b.call('GET', 'auth-value') != b'committed':
                            assert time.monotonic() < deadline
                            time.sleep(0.02)
                        assert b.call('HGET', 'auth-hash', 'f') == b'x' * 70000
                finally:
                    a.close()
                    b.close()


def long_password(binary, reference):
    password = 'x' * 4097
    with server(binary, password=password) as port, \
            server(reference, redis=True, password=password) as ref:
        a, b = Client(port), Client(ref)
        try:
            for value in [password, password[:-1] + 'y', password[:-1],
                          password + '\0', password]:
                assert a.call('AUTH', value) == b.call('AUTH', value)
            for value in [password, password[:-1] + 'y']:
                s = rpc_socket(port)
                try:
                    s.sendall(b'PKV-AUTH %d\r\n' % len(value))
                    for offset in range(0, len(value), 333):
                        s.sendall(value[offset:offset + 333].encode())
                    s.sendall(b'\r\nPKV-REPL 3\r\n')
                    if value == password:
                        stream = s.makefile('rb')
                        assert stream.read(5) == b'+OK\r\n'
                        assert len(stream.read(20)) == 20
                        stream.close()
                    else:
                        try:
                            assert s.recv(32) == b''
                        except ConnectionResetError:
                            pass
                finally:
                    s.close()
        finally:
            a.close()
            b.close()
    replication(binary, password, False)


def unavailable_source(binary):
    with socket.socket() as reserved:
        reserved.bind(('127.0.0.1', 0))
        address = '127.0.0.1:%d' % reserved.getsockname()[1]
        with server(binary, password=PASSWORD, replica_rpc_source=address,
                    masterauth=PASSWORD) as target:
            c = Client(target)
            try:
                assert c.call('AUTH', PASSWORD) == b'OK'
                for _ in range(5):
                    assert c.call('PING') == b'PONG'
                    time.sleep(0.3)
            finally:
                c.close()


def cli(binary, port, reference):
    executable = Path(binary).with_name('redis-cli')
    assert executable.is_file()
    env = os.environ.copy()
    env['REDISCLI_AUTH'] = PASSWORD
    request = b'PING\nCLIENT SETNAME cli-auth\nRESET\nPING\n'
    request += b'AUTH default ' + PASSWORD.encode() + b'\nPING\n'
    results = []
    for selected in [port, reference]:
        result = subprocess.run([str(executable), '-3', '--raw', '-h',
                                 '127.0.0.1', '-p', str(selected)],
                                input=request, capture_output=True,
                                timeout=10, env=env)
        assert result.stdout.count(b'PONG') == 2, result
        assert b'NOAUTH' in result.stdout + result.stderr, result
        results.append((result.returncode, result.stdout, result.stderr))
    assert results[0] == results[1], results
    print('redis-cli authenticated RESP3 and RESET passed')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--pkv', required=True)
    parser.add_argument('--redis', required=True)
    args = parser.parse_args()
    with server(args.redis, redis=True, password=PASSWORD) as ref, \
            server(args.pkv, password=PASSWORD) as port:
        checks = compare_clients(port, ref, PASSWORD)
        handshake(port, ref, PASSWORD)
        watching(port, ref, PASSWORD)
        strict_framing(port, PASSWORD)
        cli(args.redis, port, ref)
    with server(args.redis, redis=True) as ref, server(args.pkv) as port:
        open_server(port, ref)
    rpc(args.pkv)
    replication(args.pkv)
    unavailable_source(args.pkv)
    long_password(args.pkv, args.redis)
    print('AUTH common differential assertions:', checks)


if __name__ == '__main__':
    main()
