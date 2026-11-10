#!/usr/bin/env python3
"""Socket-level Redis compatibility checks; no third-party Python packages."""
import argparse
import binascii
import contextlib
from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import random
import socket
import subprocess
import sys
import tempfile
import threading
import time


class ErrorReply(bytes):
    pass


class Client:
    def __init__(self, port):
        self.socket = socket.create_connection(('127.0.0.1', port), timeout=3)
        self.stream = self.socket.makefile('rb')

    def close(self):
        self.stream.close()
        self.socket.close()

    @staticmethod
    def encode(args):
        args = [a if isinstance(a, bytes) else str(a).encode() for a in args]
        return b'*%d\r\n' % len(args) + b''.join(b'$%d\r\n' % len(a) + a + b'\r\n' for a in args)

    def read(self):
        line = self.stream.readline()
        if not line.endswith(b'\r\n'):
            raise AssertionError('connection closed or invalid frame: %r' % line)
        kind, data = line[:1], line[1:-2]
        if kind == b'+': return data
        if kind == b'-': return ErrorReply(data)
        if kind == b':': return int(data)
        if kind == b'$':
            size = int(data)
            if size == -1: return None
            value = self.stream.read(size)
            assert len(value) == size and self.stream.read(2) == b'\r\n'
            return value
        if kind == b'*':
            size = int(data)
            return None if size == -1 else [self.read() for _ in range(size)]
        raise AssertionError('invalid reply: %r' % line)

    def call(self, *args):
        self.socket.sendall(self.encode(args))
        return self.read()


def ports():
    """Select the client and pkv's companion RPC port, both on loopback."""
    for _ in range(100):
        port = random.randrange(20000, 40000)
        with contextlib.ExitStack() as stack:
            try:
                for candidate in (port, port + 10000):
                    s = stack.enter_context(socket.socket())
                    s.bind(('127.0.0.1', candidate))
            except OSError:
                continue
            return port
    raise RuntimeError('cannot find free test ports')


def stop_server(process, log, preserving_error=False):
    """A passing command assertion must not hide an abnormal server exit."""
    if process.poll() is None:
        process.terminate()
    forced = False
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        forced = True
        process.kill()
        process.wait(timeout=3)
    if process.returncode == 0 and not forced:
        return
    log.flush()
    log.seek(0)
    detail = log.read()[-8000:]
    message = ('server shutdown failed: returncode=%s forced=%s\n%s' %
               (process.returncode, forced, detail))
    if preserving_error:
        print(message)
    else:
        raise AssertionError(message)


@contextlib.contextmanager
def server(binary, redis=False, cluster=False, databases=None,
           replica_rpc_source=None, replica_limits=None, backend=None,
           active_expire=None, password=None, masterauth=None, aclfile=None,
           login=None, acllog_max_len=None, storage_path=None,
           replica_read_only=None, process_info=None,
           move_journal_path=None, fiber_threads=2, inherited_fds=(),
           stdin=None, startup_env=None):
    port = ports()
    with tempfile.TemporaryDirectory(prefix='pkv-integration-') as directory:
        root = Path(directory)
        if redis:
            command = [binary, '--bind', '127.0.0.1', '--port', str(port),
                       '--save', '', '--appendonly', 'no', '--protected-mode', 'yes']
            if password is not None:
                command += ['--requirepass', password]
            if aclfile is not None:
                command += ['--aclfile', str(aclfile)]
            if databases is not None:
                command += ['--databases', str(databases)]
            if acllog_max_len is not None:
                command += ['--acllog-max-len', str(acllog_max_len)]
            if replica_read_only is not None:
                command += ['--replica-read-only',
                            'yes' if replica_read_only else 'no']
            if cluster:
                command += ['--cluster-enabled', 'yes', '--cluster-config-file', str(root / 'nodes.conf')]
        else:
            config = root / 'pkv.cf'
            config.write_text('service pkv\n{\n'
                              'master_service = 127.0.0.1|%d\n'
                              'master_type = sock\nmaster_reuseport = yes\nfiber_threads = %d\n'
                              'ocache_max = 100\ndb_count = 16\ndbtype = mdb\n'
                              'service = 127.0.0.1:%d\ncluster_mode = %s\n}\n' % (port, fiber_threads, port, '1' if cluster else '0'))
            if databases is not None:
                config.write_text(config.read_text().replace('dbtype = mdb\n', 'dbtype = mdb\ndatabases = %d\n' % databases))
            if replica_rpc_source is not None:
                config.write_text(config.read_text().replace('dbtype = mdb\n', 'dbtype = mdb\nreplica_rpc_source = %s\n' % replica_rpc_source))
            if replica_limits is not None:
                lines = ''.join(
                    '%s = %d\n' % (key, value)
                    for key, value in replica_limits.items())
                config.write_text(config.read_text().replace(
                    'dbtype = mdb\n', 'dbtype = mdb\n' + lines))
            if active_expire is not None:
                config.write_text(config.read_text().replace(
                    'dbtype = mdb\n', 'dbtype = mdb\nactive_expire = %d\n' %
                    bool(active_expire)))
            if replica_read_only is not None:
                config.write_text(config.read_text().replace(
                    'service pkv\n{\n', 'service pkv\n{\n' +
                    'replica_read_only = %d\n' % bool(replica_read_only)))
            if move_journal_path is not None:
                value = str(move_journal_path)
                assert not any(ch in value for ch in '\r\n\0')
                config.write_text(config.read_text().replace(
                    'service pkv\n{\n', 'service pkv\n{\n' +
                    'move_journal_path = %s\n' % value))
            selected = backend or os.environ.get('PKV_TEST_BACKEND', 'mdb')
            if selected not in ('mdb', 'mdb_tbb', 'rdb', 'wdb'):
                raise ValueError('unsupported test backend: %s' % selected)
            if selected in ('rdb', 'wdb'):
                storage = storage_path or (root / 'storage')
                assert not any(ch in str(storage) for ch in '\r\n\0')
                config.write_text(config.read_text().replace(
                    'dbtype = mdb\n', 'dbtype = mdb\ndbpath = %s\n' %
                    storage))
            config.write_text(config.read_text().replace(
                'dbtype = mdb\n', 'dbtype = %s\n' % selected))
            for name, value in [('requirepass', password),
                                ('masterauth', masterauth)]:
                if value is not None:
                    assert not any(ch in value for ch in '\r\n\0')
                    config.write_text(config.read_text().replace(
                        'service pkv\n{\n',
                        'service pkv\n{\n%s = %s\n' % (name, value)))
            if aclfile is not None:
                path = str(aclfile)
                assert not any(ch in path for ch in '\r\n\0')
                config.write_text(config.read_text().replace(
                    'service pkv\n{\n',
                    'service pkv\n{\naclfile = %s\n' % path))
            if acllog_max_len is not None:
                config.write_text(config.read_text().replace(
                    'service pkv\n{\n',
                    'service pkv\n{\nacllog-max-len = %d\n' %
                    acllog_max_len))
            command = [binary, 'alone', str(config)]
        with (root / 'server.log').open('w+') as log:
            env = os.environ.copy()
            env.update(startup_env or {})
            env['UBSAN_OPTIONS'] = env.get('UBSAN_OPTIONS', '') + ':halt_on_error=1'
            process = subprocess.Popen(command, cwd=directory, stdout=log,
                                       stderr=subprocess.STDOUT, env=env,
                                       pass_fds=inherited_fds, stdin=stdin)
            try:
                if process_info is not None:
                    process_info['pid'] = process.pid
                    process_info['process'] = process
                deadline = time.monotonic() + 8
                while time.monotonic() < deadline:
                    if process.poll() is not None:
                        raise AssertionError('server exited with %s' % process.returncode)
                    try:
                        client = Client(port)
                        try:
                            if login is not None:
                                assert client.call('AUTH', *login) == b'OK'
                            elif password:
                                assert client.call('AUTH', password) == b'OK'
                            assert client.call('PING') == b'PONG'
                        finally:
                            client.close()
                        break
                    except (ConnectionRefusedError, ConnectionResetError,
                            BrokenPipeError, TimeoutError):
                        # ACL can listen before startup validation rejects the
                        # config. Recheck the child status after socket closure.
                        time.sleep(.03)
                    except AssertionError as error:
                        if str(error) != (
                                "connection closed or invalid frame: b''"):
                            raise
                        time.sleep(.03)
                else:
                    raise AssertionError('server startup timeout')
                yield port
            except BaseException:
                log.flush()
                log.seek(0)
                print(log.read()[-8000:])
                raise
            finally:
                stop_server(process, log, sys.exc_info()[0] is not None)
                if process_info is not None:
                    log.flush()
                    log.seek(0)
                    process_info['log_text'] = log.read()


def run(port):
    count = 0
    c = Client(port)
    def check(expected, *args):
        nonlocal count
        actual = c.call(*args)
        assert type(actual) is type(expected) and actual == expected, (args, actual, expected)
        count += 1

    def error(prefix, *args):
        nonlocal count
        actual = c.call(*args)
        assert isinstance(actual, ErrorReply) and actual.startswith(prefix), (args, actual)
        count += 1

    try:
        check(b'PONG', 'PING')
        check(b'', 'PING', b'')
        check(b'a\0b', 'ECHO', b'a\0b')
        check(b'OK', 'SET', 'smoke', 'value')
        check(b'value', 'GET', 'smoke')
        check(None, 'GET', 'missing')
        check(b'OK', 'SET', b'', b'')
        check(b'', 'GET', b'')
        check(b'OK', 'sEt', 'lowercase', 'mixed')
        check(b'mixed', 'gEt', 'lowercase')
        # Every byte, including invalid UTF-8, must survive argument and JSON storage paths.
        samples = [bytes(range(256)), b'\0', b'\xff\xfe\xc0\xaf', b'\r\n', b'']
        rng = random.Random(0x504b56)
        samples += [bytes(rng.randrange(256) for _ in range(rng.randrange(1, 1024))) for _ in range(50)]
        for i, value in enumerate(samples):
            key = b'bytes:' + str(i).encode() + b'\0' + bytes(range(256))
            check(b'OK', 'SET', key, value)
            check(value, 'GET', key)
            check(None, 'GET', key.split(b'\0')[0])
            check(value, 'ECHO', value)
        for command in [('GET',), ('GET', 'smoke', 'extra'), ('SET', 'smoke'),
                        ('PING', 'one', 'two'), ('ECHO',), ('ECHO', 'one', 'two')]:
            error(b'ERR wrong number of arguments', *command)
            check(b'PONG', 'PING')
        error(b'ERR syntax error', 'SET', 'smoke', 'changed', 'unknown-option')
        check(b'value', 'GET', 'smoke')
        error(b'ERR unknown command', b'bad\r\n+INJECTED')
        check(b'PONG', 'PING')
        check(1, 'HSET', 'hash', 'field', 'value')
        error(b'WRONGTYPE ', 'GET', 'hash')
        check(b'PONG', 'PING')
        check(b'none', 'TYPE', 'absent-type')
        check(b'string', 'TYPE', 'smoke')
        check(b'hash', 'TYPE', 'hash')
        check(0, 'DEL', 'absent-delete')
        check(b'OK', 'SET', 'del-one', '1')
        check(b'OK', 'SET', 'del-two', '2')
        check(2, 'DEL', 'del-one', 'del-one', 'del-two', 'absent-delete')
        check(None, 'GET', 'del-two')
        check(1, 'DEL', b'')
        check(b'none', 'TYPE', b'')
        check(-2, 'TTL', 'absent-expiry')
        check(0, 'EXPIRE', 'absent-expiry', 10)
        check(b'OK', 'SET', 'expiry', 'value')
        check(-1, 'TTL', 'expiry')
        for invalid in (b'', b'01', b'+1', b'-0', b' 1', b'1\0x', b'1.5', b'9223372036854775808'):
            error(b'ERR value is not an integer or out of range', 'EXPIRE', 'expiry', invalid)
        error(b'ERR invalid expire time', 'EXPIRE', 'expiry', '9223372036854775807')
        check(1, 'EXPIRE', 'expiry', 3000000000)
        ttl = c.call('TTL', 'expiry')
        assert isinstance(ttl, int) and 2999999999 <= ttl <= 3000000000
        count += 1
        check(1, 'EXPIRE', 'expiry', 0)
        check(None, 'GET', 'expiry')
        check(b'none', 'TYPE', 'expiry')
        check(0, 'DEL', 'expiry')
        check(b'OK', 'SET', 'negative-expiry', 'v')
        check(1, 'EXPIRE', 'negative-expiry', -1)
        check(-2, 'TTL', 'negative-expiry')
        check(0, 'EXPIRE', 'negative-expiry', -1)
        check(None, 'HGET', 'absent-hash', 'field')
        check([], 'HGETALL', 'absent-hash')
        check([None, None, None], 'HMGET', 'absent-hash', 'b', 'a', 'b')
        check(0, 'HDEL', 'absent-hash', 'a', 'b')
        hk = b'hash\0' + bytes(range(256))
        hf = bytes(range(256))
        check(2, 'HSET', hk, hf, hf, b'', b'')
        check(0, 'HSET', hk, hf, b'updated\0value')
        check(b'updated\0value', 'HGET', hk, hf)
        check(b'', 'HGET', hk, b'')
        check(None, 'HGET', hk, b'not-there')
        check([b'updated\0value', None, b'', b'updated\0value'], 'HMGET', hk, hf, 'missing', b'', hf)
        check(1, 'HSET', hk, 'duplicate', 'first', 'duplicate', 'last')
        check(b'last', 'HGET', hk, 'duplicate')
        check(b'OK', 'HMSET', hk, 'z', 'last', 'z', 'final', 'a', 'alpha')
        all_fields = c.call('HGETALL', hk)
        assert len(all_fields) == 10 and dict(zip(all_fields[::2], all_fields[1::2])) == {
            hf: b'updated\0value', b'': b'', b'duplicate': b'last', b'z': b'final', b'a': b'alpha'}
        count += 1
        for command in ('HSET', 'HMSET'):
            error(b'ERR wrong number of arguments', command, hk, 'field', 'value', 'incomplete')
            check(None, 'HGET', hk, 'field')
        check(1, 'EXPIRE', hk, 60)
        check(1, 'HSET', hk, 'ttl-field', 'retained')
        check(1, 'HDEL', hk, 'duplicate', 'duplicate', 'absent')
        ttl = c.call('TTL', hk)
        assert 59 <= ttl <= 60, ttl
        count += 1
        check(5, 'HDEL', hk, hf, b'', 'a', 'z', 'ttl-field')
        check(b'none', 'TYPE', hk)
        check([], 'HGETALL', hk)
        check(1, 'HSET', b'', b'', b'')
        check(b'', 'HGET', b'', b'')
        check(1, 'DEL', b'')
        for command in [('HSET', 'smoke', 'a', 'bad'), ('HMSET', 'smoke', 'a', 'bad'),
                        ('HGET', 'smoke', 'a'), ('HGETALL', 'smoke'), ('HMGET', 'smoke', 'a'),
                        ('HDEL', 'smoke', 'a')]:
            error(b'WRONGTYPE ', *command)
            check(b'value', 'GET', 'smoke')
        # Hash reads, conditional creation and signed integer updates.
        for cmd in ('HLEN', 'HSTRLEN', 'HEXISTS', 'HKEYS', 'HVALS'):
            args = ('hash-new-missing', 'field') if cmd in ('HSTRLEN', 'HEXISTS') else ('hash-new-missing',)
            check([] if cmd in ('HKEYS', 'HVALS') else 0, cmd, *args)
        deadline = 4102444800123
        nh = b'hash-extended\0'
        check(1, 'HSETNX', nh, b'', b'')
        check(0, 'HSETNX', nh, b'', 'replace')
        check(b'', 'HGET', nh, b'')
        check(1, 'HEXISTS', nh, b'')
        check(0, 'HSTRLEN', nh, b'')
        check(1, 'HSETNX', nh, hf, hf)
        check(256, 'HSTRLEN', nh, hf)
        check(2, 'HLEN', nh)
        names = c.call('HKEYS', nh)
        values = c.call('HVALS', nh)
        assert sorted(names) == sorted([b'', hf]) and sorted(values) == sorted([b'', hf])
        count += 2
        check(0, 'HINCRBY', nh, 'counter', 0)
        check(b'0', 'HGET', nh, 'counter')
        check(1, 'HINCRBY', nh, 'counter', 1)
        check(-2, 'HINCRBY', nh, 'counter', -3)
        check(1, 'PEXPIREAT', nh, deadline)
        check(3, 'HINCRBY', nh, 'counter', 5)
        check(deadline, 'PEXPIRETIME', nh)
        check(0, 'HSETNX', nh, 'counter', 'ignored')
        check(deadline, 'PEXPIRETIME', nh)
        check(1, 'HSETNX', nh, 'new', 'new')
        check(deadline, 'PEXPIRETIME', nh)
        for index, (old, delta) in enumerate([('9223372036854775807', 1), ('-9223372036854775808', -1)]):
            check(1 if index == 0 else 0, 'HSET', nh, 'limit', old)
            error(b'ERR increment or decrement would overflow', 'HINCRBY', nh, 'limit', delta)
            check(old.encode(), 'HGET', nh, 'limit')
            check(deadline, 'PEXPIRETIME', nh)
        check(0, 'HSET', nh, 'limit', '0')
        check(-9223372036854775808, 'HINCRBY', nh, 'limit', '-9223372036854775808')
        check(-1, 'HINCRBY', nh, 'limit', '9223372036854775807')
        for index, bad in enumerate((b'', b'+1', b'01', b'-0', b' 1', b'1.0', b'1\0', b'9223372036854775808')):
            check(1 if index == 0 else 0, 'HSET', nh, 'bad', bad)
            error(b'ERR hash value is not an integer', 'HINCRBY', nh, 'bad', 1)
            check(bad, 'HGET', nh, 'bad')
            error(b'ERR value is not an integer or out of range', 'HINCRBY', nh, 'new-field', bad)
            check(0, 'HEXISTS', nh, 'new-field')
            check(deadline, 'PEXPIRETIME', nh)
        for cmd, args in [('HSETNX', ('f', 'v')), ('HINCRBY', ('f', 1)), ('HLEN', ()),
                          ('HSTRLEN', ('f',)), ('HEXISTS', ('f',)), ('HKEYS', ()), ('HVALS', ())]:
            error(b'WRONGTYPE ', cmd, 'smoke', *args)
            error(b'ERR wrong number of arguments', cmd)
            error(b'ERR wrong number of arguments', cmd, nh, *args, 'extra')
        error(b'ERR value is not an integer or out of range', 'HINCRBY', 'smoke', 'f', 'bad')
        check(1, 'HSET', 'hash-expired-new', 'old', '1')
        check(1, 'PEXPIREAT', 'hash-expired-new', 1)
        check(1, 'HSETNX', 'hash-expired-new', 'fresh', 'v')
        check(1, 'HLEN', 'hash-expired-new')
        check(-1, 'PTTL', 'hash-expired-new')
        check(1, 'PEXPIREAT', 'hash-expired-new', 1)
        check(5, 'HINCRBY', 'hash-expired-new', 'fresh', 5)
        check(-1, 'PTTL', 'hash-expired-new')
        # SET conditions and exchange replies must be evaluated atomically.
        check(None, 'SET', 'conditional', 'absent', 'XX')
        check(b'OK', 'SET', 'conditional', 'first', 'NX')
        check(None, 'SET', 'conditional', 'bad', 'NX')
        check(b'first', 'SET', 'conditional', 'bad', 'NX', 'GET')
        check(b'first', 'GET', 'conditional')
        check(b'first', 'SET', 'conditional', 'second', 'XX', 'GET')
        check(b'second', 'SET', 'conditional', b'', 'GET')
        check(b'', 'SET', 'conditional', 'third', 'GET', 'XX')
        check(None, 'SET', 'get-creates', 'new', 'GET')
        check(b'new', 'GET', 'get-creates')
        check(None, 'SET', 'xx-missing', 'bad', 'GET', 'XX')
        check(None, 'GET', 'xx-missing')
        check(b'OK', 'SET', 'duplicates', 'value', 'NX', 'NX', 'EX', 'not-an-int', 'EX', '60')
        check(b'value', 'GET', 'duplicates')
        for flags in [('NX', 'XX'), ('EX', 10, 'PX', 10), ('EX', 10, 'KEEPTTL'),
                      ('KEEPTTL', 'PX', 10), ('EX',), ('PXAT',)]:
            error(b'ERR syntax error', 'SET', 'conditional', 'invalid', *flags)
            check(b'third', 'GET', 'conditional')
        for amount in (0, -1, '9223372036854775807'):
            error(b'ERR invalid expire time', 'SET', 'conditional', 'invalid', 'EX', amount)
        error(b'ERR value is not an integer', 'SET', 'conditional', 'invalid', 'PX', b'1\0x')
        error(b'WRONGTYPE ', 'SET', 'hash', 'bad', 'GET')
        error(b'WRONGTYPE ', 'SET', 'hash', 'bad', 'NX', 'GET')
        check(b'hash', 'TYPE', 'hash')
        # Absolute millisecond expiry gives an exact, clock-independent record assertion.
        deadline = (int(time.time()) + 120) * 1000 + 123
        check(b'OK', 'SET', 'precision', 'value', 'PXAT', deadline)
        check(deadline, 'PEXPIRETIME', 'precision')
        check((deadline + 500) // 1000, 'EXPIRETIME', 'precision')
        before = int(time.time() * 1000)
        remaining = c.call('PTTL', 'precision')
        after = int(time.time() * 1000)
        assert deadline - after - 2 <= remaining <= deadline - before + 2, remaining
        count += 1
        check(b'OK', 'SET', 'precision', 'retained', 'KEEPTTL')
        check(deadline, 'PEXPIRETIME', 'precision')
        check(1, 'HSET', 'hash-precision', 'field', 'value')
        check(1, 'PEXPIREAT', 'hash-precision', deadline)
        check(1, 'HSET', 'hash-precision', 'new', 'value')
        check(deadline, 'PEXPIRETIME', 'hash-precision')
        check(1, 'PERSIST', 'hash-precision')
        check(-1, 'PTTL', 'hash-precision')
        check(0, 'PERSIST', 'hash-precision')
        check(b'OK', 'SET', 'precision', 'cleared')
        check(-1, 'PTTL', 'precision')
        check(-1, 'PEXPIRETIME', 'precision')
        check(0, 'PERSIST', 'absent-persist')
        check(-2, 'PEXPIRETIME', 'absent-persist')
        check(b'OK', 'SET', 'past', 'old')
        check(b'old', 'SET', 'past', 'new', 'GET', 'PXAT', 1)
        check(None, 'GET', 'past')
        check(b'OK', 'SET', 'kept-missing', 'v', 'KEEPTTL')
        check(-1, 'PTTL', 'kept-missing')
        check(b'OK', 'SET', 'short-lived', 'v', 'PX', 200)
        check(0, 'TTL', 'short-lived')
        remaining = c.call('PTTL', 'short-lived')
        assert 0 <= remaining <= 200, remaining
        count += 1
        time.sleep(.25)
        check(None, 'GET', 'short-lived')
        check(-2, 'PTTL', 'short-lived')
        check(b'OK', 'SET', 'expire-conditions', 'v')
        check(0, 'PEXPIREAT', 'expire-conditions', deadline, 'XX')
        check(0, 'PEXPIREAT', 'expire-conditions', deadline, 'GT')
        check(1, 'PEXPIREAT', 'expire-conditions', deadline, 'LT')
        check(0, 'PEXPIREAT', 'expire-conditions', deadline, 'NX')
        check(0, 'PEXPIREAT', 'expire-conditions', deadline, 'GT')
        check(0, 'PEXPIREAT', 'expire-conditions', deadline, 'LT')
        check(1, 'PEXPIREAT', 'expire-conditions', deadline + 1, 'XX', 'GT')
        check(deadline + 1, 'PEXPIRETIME', 'expire-conditions')
        check(1, 'PEXPIREAT', 'expire-conditions', deadline, 'XX', 'LT')
        check(1, 'PERSIST', 'expire-conditions')
        check(1, 'PEXPIREAT', 'expire-conditions', deadline, 'NX', 'NX')
        for flags in [('NX', 'XX'), ('NX', 'GT'), ('NX', 'LT'), ('GT', 'LT')]:
            error(b'ERR ', 'PEXPIRE', 'expire-conditions', 100, *flags)
            check(deadline, 'PEXPIRETIME', 'expire-conditions')
        error(b'ERR Unsupported option', 'EXPIRE', 'expire-conditions', 10, 'bad-option')
        check(1, 'PEXPIRE', 'expire-conditions', 0, 'XX')
        check(None, 'GET', 'expire-conditions')
        check(0, 'PEXPIRE', 'expire-conditions', 0, 'XX')
        check(b'OK', 'SET', 'second-absolute', 'v', 'EXAT', deadline // 1000)
        check((deadline // 1000) * 1000, 'PEXPIRETIME', 'second-absolute')
        check(1, 'EXPIREAT', 'second-absolute', deadline // 1000 + 1)
        check((deadline // 1000 + 1) * 1000, 'PEXPIRETIME', 'second-absolute')
        check(b'OK', 'MSET', 'multi-a', 'one', 'multi-b', 'two')
        check([b'two', b'one', None, b'two'], 'MGET', 'multi-b', 'multi-a', 'multi-missing', 'multi-b')
        check(b'OK', 'MSET', b'', b'', b'multi\0key', bytes(range(256)))
        check([b'', bytes(range(256)), None], 'MGET', b'', b'multi\0key', 'hash')
        check(4, 'EXISTS', b'', b'multi\0key', 'hash', 'hash', 'multi-missing')
        check(b'OK', 'MSET', 'duplicate-mset', 'first', 'duplicate-mset', 'last')
        check(b'last', 'GET', 'duplicate-mset')
        check(1, 'MSETNX', 'duplicate-nx', 'first', 'duplicate-nx', 'last')
        check(b'last', 'GET', 'duplicate-nx')
        check(0, 'MSETNX', 'multi-a', 'bad', 'nx-not-created', 'bad')
        check(None, 'GET', 'nx-not-created')
        check(b'one', 'GET', 'multi-a')
        check(1, 'MSETNX', 'nx-created-one', 'one', 'nx-created-two', 'two')
        check([b'one', b'two'], 'MGET', 'nx-created-one', 'nx-created-two')
        for command in [('MGET',), ('MSET',), ('MSET', 'multi-a'), ('MSET', 'multi-a', 'bad', 'dangling'),
                        ('MSETNX', 'multi-a', 'bad', 'dangling'), ('EXISTS',)]:
            error(b'ERR wrong number of arguments', *command)
            check(b'one', 'GET', 'multi-a')
        check(1, 'PEXPIRE', 'multi-a', 60000)
        check(1, 'PEXPIRE', 'multi-b', 60000)
        check(b'OK', 'MSET', 'multi-a', 'fresh-a', 'multi-b', 'fresh-b')
        check(-1, 'PTTL', 'multi-a')
        check(-1, 'PTTL', 'multi-b')
        check(1, 'HSET', 'mset-overwrite-hash', 'field', 'value')
        check(b'OK', 'MSET', 'mset-overwrite-hash', 'now-string', 'multi-empty', b'')
        check([b'now-string', b''], 'MGET', 'mset-overwrite-hash', 'multi-empty')
        check(b'string', 'TYPE', 'mset-overwrite-hash')
        check(2, 'DEL', 'multi-a', 'multi-b', 'multi-a')
        check([None, None], 'MGET', 'multi-a', 'multi-b')
        check(0, 'EXISTS', 'multi-a', 'multi-a', 'multi-b')
        check(1, 'DEL', b'')
        error(b'ERR This instance has cluster support disabled', 'CLUSTER', 'KEYSLOT', 'a')
        check(1, 'INCR', 'integer-new')
        check(0, 'DECR', 'integer-new')
        check(7, 'INCRBY', 'integer-new', 7)
        check(10, 'DECRBY', 'integer-new', -3)
        check(b'10', 'GET', 'integer-new')
        check(1, 'PEXPIREAT', 'integer-new', deadline)
        check(11, 'INCR', 'integer-new')
        check(deadline, 'PEXPIRETIME', 'integer-new')
        pipeline_key = b'integer\0pipeline'
        requests = [('SET', pipeline_key, 0, 'PXAT', deadline)]
        requests += [('incr', pipeline_key)] * 17
        requests += [('DECR', pipeline_key), ('PEXPIRETIME', pipeline_key),
                     ('SET', pipeline_key, 9223372036854775807),
                     ('INCR', pipeline_key), ('DECR', pipeline_key),
                     ('GET', pipeline_key)]
        c.socket.sendall(b''.join(c.encode(args) for args in requests))
        replies = [c.read() for _ in requests]
        assert replies[:21] == [b'OK'] + list(range(1, 18)) + [16, deadline, b'OK']
        assert isinstance(replies[21], ErrorReply) and b'overflow' in replies[21]
        assert replies[22:] == [9223372036854775806, b'9223372036854775806']
        count += len(requests)
        stream_key = b'stream\0pipeline'
        requests = [('xadd', stream_key, f'1-{i}', b'f\0', str(i))
                    for i in range(1, 18)]
        requests += [('XADD', stream_key, '1-17', 'f', 'duplicate'),
                     ('XADD', stream_key, '1-18', b'f\0', '18'),
                     ('XLEN', stream_key), ('XRANGE', stream_key, '1-16', '1-18'),
                     ('DEL', stream_key)]
        c.socket.sendall(b''.join(c.encode(args) for args in requests))
        replies = [c.read() for _ in requests]
        assert replies[:17] == [f'1-{i}'.encode() for i in range(1, 18)]
        assert isinstance(replies[17], ErrorReply) and b'equal or smaller' in replies[17]
        assert replies[18:] == [b'1-18', 18,
            [[f'1-{i}'.encode(), [b'f\0', str(i).encode()]] for i in range(16, 19)], 1]
        count += len(requests)
        check(-9223372036854775808, 'INCRBY', 'integer-min', '-9223372036854775808')
        error(b'ERR increment or decrement would overflow', 'DECR', 'integer-min')
        check(b'-9223372036854775808', 'GET', 'integer-min')
        check(b'OK', 'SET', 'integer-max', '9223372036854775807')
        error(b'ERR increment or decrement would overflow', 'INCR', 'integer-max')
        check(b'9223372036854775807', 'GET', 'integer-max')
        error(b'ERR decrement would overflow', 'DECRBY', 'decr-missing', '-9223372036854775808')
        check(None, 'GET', 'decr-missing')
        error(b'ERR decrement would overflow', 'DECRBY', 'hash', '-9223372036854775808')
        for value in (b'', b'01', b'-0', b'+1', b'1.0', b' 1', b'1\0x', b'9223372036854775808'):
            check(b'OK', 'SET', 'bad-integer', value, 'PXAT', deadline)
            error(b'ERR value is not an integer or out of range', 'INCR', 'bad-integer')
            check(value, 'GET', 'bad-integer')
            check(deadline, 'PEXPIRETIME', 'bad-integer')
        for value in ('01', '+1', '-0', '1.1', '9223372036854775808'):
            error(b'ERR value is not an integer or out of range', 'INCRBY', 'integer-new', value)
        check(0, 'APPEND', 'empty-append', b'')
        check(b'', 'GET', 'empty-append')
        check(1, 'EXISTS', 'empty-append')
        check(3, 'APPEND', 'binary-append', b'a\0b')
        check(259, 'APPEND', 'binary-append', bytes(range(256)))
        check(b'a\0b' + bytes(range(256)), 'GET', 'binary-append')
        check(259, 'STRLEN', 'binary-append')
        check(0, 'STRLEN', 'strlen-absent')
        check(0, 'SETRANGE', 'empty-range', '9223372036854775807', b'')
        check(None, 'GET', 'empty-range')
        check(6, 'SETRANGE', 'range-gap', 4, b'a\0')
        check(b'\0\0\0\0a\0', 'GET', 'range-gap')
        check(6, 'SETRANGE', 'range-gap', 1, b'bc')
        check(b'\0bc\0a\0', 'GET', 'range-gap')
        check(1, 'PEXPIREAT', 'range-gap', deadline)
        check(6, 'SETRANGE', 'range-gap', '9223372036854775807', b'')
        check(deadline, 'PEXPIRETIME', 'range-gap')
        check(7, 'APPEND', 'range-gap', b'z')
        check(deadline, 'PEXPIRETIME', 'range-gap')
        for offset in (-1, '-9223372036854775808'):
            error(b'ERR offset is out of range', 'SETRANGE', 'hash', offset, b'')
        for offset in (536870912, '9223372036854775807'):
            error(b'ERR string exceeds maximum allowed size', 'SETRANGE', 'range-too-large', offset, b'x')
        check(None, 'GET', 'range-too-large')
        check(b'OK', 'SET', 'substring', bytes(range(256)))
        ranges = [(0, -1, bytes(range(256))), (-3, -1, bytes(range(253, 256))), (0, 0, b'\0'),
                  (-1, -2, b''), (-9223372036854775808, -9223372036854775808, b'\0'),
                  (0, -9223372036854775808, b'\0'), (9223372036854775807, 9223372036854775807, b''),
                  (-9223372036854775808, 9223372036854775807, bytes(range(256))), (250, 300, bytes(range(250, 256)))]
        for start_index, end_index, expected in ranges:
            check(expected, 'GETRANGE', 'substring', start_index, end_index)
            check(expected, 'SUBSTR', 'substring', start_index, end_index)
        check(b'', 'GETRANGE', 'range-absent', 0, -1)
        check(None, 'GETDEL', 'getdel-absent')
        check(b'OK', 'SET', 'getdel-empty', b'')
        check(b'', 'GETDEL', 'getdel-empty')
        check(None, 'GETDEL', 'getdel-empty')
        check(None, 'GETSET', 'getset-value', 'first')
        check(1, 'PEXPIREAT', 'getset-value', deadline)
        check(b'first', 'GETSET', 'getset-value', b'second\0')
        check(-1, 'PTTL', 'getset-value')
        check(b'second\0', 'GET', 'getset-value')
        check(1, 'SETNX', 'setnx-alias', 'first')
        check(0, 'SETNX', 'setnx-alias', 'second')
        check(b'first', 'GET', 'setnx-alias')
        check(0, 'SETNX', 'hash', 'bad')
        check(b'OK', 'SETEX', 'setex-alias', 60, b'seconds\0')
        check(b'seconds\0', 'GET', 'setex-alias')
        remaining = c.call('PTTL', 'setex-alias')
        assert 59000 <= remaining <= 60000, remaining
        count += 1
        check(b'OK', 'PSETEX', 'psetex-alias', 60000, b'milliseconds\0')
        check(b'milliseconds\0', 'GET', 'psetex-alias')
        for name in ('SETEX', 'PSETEX'):
            error(b'ERR invalid expire time', name, 'invalid-expiry-alias', 0, 'bad')
            if name == 'SETEX':
                error(b'ERR invalid expire time', name, 'invalid-expiry-alias', '9223372036854775807', 'bad')
        check(None, 'GET', 'invalid-expiry-alias')
        check(None, 'GETEX', 'getex-absent', 'PX', 'not-an-integer')
        check(None, 'GETEX', 'getex-absent', 'PX', 0)
        error(b'ERR syntax error', 'GETEX', 'getex-absent', 'invalid')
        check(b'OK', 'SET', 'getex-value', b'value\0')
        check(b'value\0', 'GETEX', 'getex-value', 'PXAT', deadline)
        check(deadline, 'PEXPIRETIME', 'getex-value')
        check(b'value\0', 'GETEX', 'getex-value')
        check(deadline, 'PEXPIRETIME', 'getex-value')
        error(b'ERR value is not an integer', 'GETEX', 'getex-value', 'PX', 'not-an-integer')
        error(b'ERR invalid expire time', 'GETEX', 'getex-value', 'EX', 0)
        check(deadline, 'PEXPIRETIME', 'getex-value')
        check(b'value\0', 'GETEX', 'getex-value', 'PERSIST')
        check(-1, 'PTTL', 'getex-value')
        check(b'value\0', 'GETEX', 'getex-value', 'PXAT', 1)
        check(None, 'GET', 'getex-value')
        for command in [('INCR', 'hash'), ('APPEND', 'hash', b''), ('STRLEN', 'hash'),
                        ('GETRANGE', 'hash', 0, -1), ('SETRANGE', 'hash', 0, b''),
                        ('GETEX', 'hash', 'PX', 'bad'), ('GETDEL', 'hash'), ('GETSET', 'hash', 'bad')]:
            error(b'WRONGTYPE ', *command)
        check(b'hash', 'TYPE', 'hash')
        for command in [('INCR',), ('INCR', 'a', 'extra'), ('INCRBY', 'a'), ('DECRBY', 'a'), ('APPEND', 'a'),
                        ('STRLEN',), ('GETRANGE', 'a', 0), ('SETRANGE', 'a', 0), ('GETEX',), ('GETDEL', 'a', 'extra'),
                        ('GETSET', 'a'), ('SETNX', 'a'), ('SETEX', 'a', 1), ('PSETEX', 'a', 1)]:
            error(b'ERR wrong number of arguments', *command)
        # A command error must leave later pipelined commands aligned.
        commands = [('SET', 'pipeline', 'v'), ('GET',), ('GET', 'pipeline'), ('ECHO', b'')]
        c.socket.sendall(b''.join(c.encode(args) for args in commands))
        replies = [c.read() for _ in commands]
        assert replies[0] == b'OK' and isinstance(replies[1], ErrorReply) and replies[2:] == [b'v', b'']
        count += 4
        large = b'a\0' * (36 * 1024)
        check(b'OK', 'SET', 'large-pipeline', large)
        commands = [('GET', 'large-pipeline'), ('ECHO', b'between'),
                    ('GET', 'large-pipeline'), ('GET',), ('PING',),
                    ('DEL', 'large-pipeline')]
        c.socket.sendall(b''.join(c.encode(args) for args in commands))
        replies = [c.read() for _ in commands]
        assert replies[:3] == [large, b'between', large]
        assert isinstance(replies[3], ErrorReply) and replies[4:] == [b'PONG', 1]
        count += len(commands)
        wire = c.encode(('SET', b'fragmented\0key', b'a\0b\r\n')) + c.encode(('GET', b'fragmented\0key'))
        for byte in wire:
            c.socket.sendall(bytes([byte]))
        assert c.read() == b'OK' and c.read() == b'a\0b\r\n'
        count += 2
        c.socket.sendall(c.encode(('PING',)) + c.encode(('QUIT',)) + c.encode(('SET', 'after-quit', 'bad')))
        assert c.read() == b'PONG' and c.read() == b'OK'
        assert c.stream.read(1) == b''
        count += 3
    finally:
        c.close()

    def worker(number):
        client = Client(port)
        try:
            for i in range(50):
                key = ('thread:%d:%d' % (number, i)).encode() + b'\0key'
                value = bytes(range(256)) + str(i).encode()
                assert client.call('SET', key, value) == b'OK'
                assert client.call('GET', key) == value
                assert client.call('HSET', b'shared-hash\0', key, value) == 1
            return 150
        finally:
            client.close()

    with ThreadPoolExecutor(max_workers=8) as pool:
        count += sum(pool.map(worker, range(8)))
    hash_start = threading.Barrier(8)
    def hash_worker(number):
        client = Client(port)
        try:
            hash_start.wait(timeout=10)
            values = [client.call('HINCRBY', 'hash-counter-race', b'counter\0', 1) for _ in range(100)]
            wins = [client.call('HSETNX', 'hash-nx-race', str(i), str(number)) for i in range(20)]
            return values, wins
        finally:
            client.close()
    with ThreadPoolExecutor(max_workers=8) as pool:
        outcomes = list(pool.map(hash_worker, range(8)))
    assert sorted(value for values, _ in outcomes for value in values) == list(range(1, 801))
    assert all(sum(wins) == 1 for wins in zip(*(wins for _, wins in outcomes)))
    count += 962
    setup = Client(port)
    try:
        assert setup.call('SET', 'atomic-increment', '0') == b'OK'
        assert setup.call('SET', 'atomic-range', b'\0' * 1600) == b'OK'
        assert setup.call('SET', 'consume-once', b'one-shot\0value') == b'OK'
    finally:
        setup.close()
    string_start = threading.Barrier(8)
    def string_worker(number):
        client = Client(port)
        increments, appends = [], []
        try:
            string_start.wait(timeout=10)
            for _ in range(100):
                value = client.call('INCR', 'atomic-increment')
                assert type(value) is int, value
                increments.append(value)
            for i in range(30):
                token = ('%02d:%04d:' % (number, i)).encode().ljust(16, b'\0')
                length = client.call('APPEND', 'atomic-append', token)
                assert type(length) is int, length
                appends.append((length, token))
            for i in range(25):
                token = ('%02d%04d' % (number, i)).encode().ljust(8, b'_')
                assert client.call('SETRANGE', 'atomic-range', (number * 25 + i) * 8, token) == 1600
            consumed = client.call('GETDEL', 'consume-once')
            assert consumed is None or consumed == b'one-shot\0value', consumed
            return increments, appends, consumed
        finally:
            client.close()
    with ThreadPoolExecutor(max_workers=8) as pool:
        string_results = list(pool.map(string_worker, range(8)))
    increments = [value for values, _, _ in string_results for value in values]
    assert sorted(increments) == list(range(1, 801)), increments
    assert sum(consumed is not None for _, _, consumed in string_results) == 1
    final = Client(port)
    try:
        assert final.call('GET', 'atomic-increment') == b'800'
        appended = final.call('GET', 'atomic-append')
        assert len(appended) == 3840
        for _, appends, _ in string_results:
            for length, token in appends:
                assert appended[length - 16:length] == token
        expected = b''.join(('%02d%04d' % (number, i)).encode().ljust(8, b'_') for number in range(8) for i in range(25))
        assert final.call('GET', 'atomic-range') == expected
        assert final.call('GET', 'consume-once') is None
    finally:
        final.close()
    count += 1254
    # Readers must never see a mixture of values from atomic multi-key writes.
    setup = Client(port)
    try:
        assert setup.call('MSET', 'snapshot-a', 'seed', 'snapshot-b', 'seed') == b'OK'
    finally:
        setup.close()
    start = threading.Barrier(6)
    def snapshot_worker(number):
        client = Client(port)
        try:
            start.wait(timeout=10)
            if number < 2:
                for i in range(150):
                    value = ('snapshot:%d:%d' % (number, i)).encode()
                    names = ('snapshot-a', 'snapshot-b') if number == 0 else ('snapshot-b', 'snapshot-a')
                    assert client.call('MSET', names[0], value, names[1], value) == b'OK'
                return 150
            for _ in range(200):
                values = client.call('MGET', 'snapshot-b', 'snapshot-a', 'snapshot-b')
                assert len(values) == 3 and type(values[0]) is bytes and values[0] == values[1] == values[2], values
            return 200
        finally:
            client.close()
    with ThreadPoolExecutor(max_workers=6) as pool:
        count += sum(pool.map(snapshot_worker, range(6)))
    nx_batch_barrier = threading.Barrier(8)
    def nx_batch_worker(number):
        client = Client(port)
        try:
            for round_number in range(10):
                if number == 0:
                    client.call('DEL', 'batch-nx-shared', *['batch-nx-aux:%d' % i for i in range(8)])
                nx_batch_barrier.wait(timeout=10)
                token = ('%d:%d' % (round_number, number)).encode()
                reply = client.call('MSETNX', 'batch-nx-shared', token, 'batch-nx-aux:%d' % number, token)
                assert type(reply) is int and reply in (0, 1), reply
                nx_batch_barrier.wait(timeout=10)
                if number == 0:
                    values = client.call('MGET', 'batch-nx-shared', *['batch-nx-aux:%d' % i for i in range(8)])
                    assert type(values[0]) is bytes
                    assert sum(value is not None for value in values[1:]) == 1, values
                    assert all(value is None or value == values[0] for value in values[1:]), values
            return 10
        finally:
            client.close()
    with ThreadPoolExecutor(max_workers=8) as pool:
        count += sum(pool.map(nx_batch_worker, range(8))) + 10
    setup = Client(port)
    try:
        setup.call('MSET', 'delete-a', 'same', 'delete-b', 'same')
    finally:
        setup.close()
    def deletion_worker(number):
        client = Client(port)
        try:
            if number == 0:
                for i in range(100):
                    assert client.call('MSET', 'delete-a', str(i), 'delete-b', str(i)) == b'OK'
                    assert client.call('DEL', 'delete-b', 'delete-a') == 2
                return 200
            for _ in range(200):
                values = client.call('MGET', 'delete-a', 'delete-b')
                assert len(values) == 2 and values[0] == values[1], values
            return 200
        finally:
            client.close()
    with ThreadPoolExecutor(max_workers=4) as pool:
        count += sum(pool.map(deletion_worker, range(4)))
    barrier = threading.Barrier(8)
    def nx_worker(number):
        client = Client(port)
        outcomes = []
        try:
            for round_number in range(20):
                if number == 0:
                    client.call('DEL', b'nx-race\0')
                barrier.wait(timeout=10)
                reply = client.call('SET', b'nx-race\0', '%d:%d' % (number, round_number), 'NX')
                assert reply is None or type(reply) is bytes and reply == b'OK', reply
                outcomes.append(reply == b'OK')
                barrier.wait(timeout=10)
            return outcomes
        finally:
            client.close()
    with ThreadPoolExecutor(max_workers=8) as pool:
        outcomes = list(pool.map(nx_worker, range(8)))
    assert all(sum(round_results) == 1 for round_results in zip(*outcomes)), outcomes
    count += 180
    setup = Client(port)
    try:
        assert setup.call('SET', 'exchange-race', 'initial') == b'OK'
    finally:
        setup.close()
    def exchange_worker(number):
        client = Client(port)
        old_values, new_values = [], []
        try:
            for i in range(25):
                token = ('exchange:%d:%d' % (number, i)).encode()
                old = client.call('SET', 'exchange-race', token, 'GET')
                assert type(old) is bytes, old
                old_values.append(old)
                new_values.append(token)
            return old_values, new_values
        finally:
            client.close()
    with ThreadPoolExecutor(max_workers=8) as pool:
        exchanges = list(pool.map(exchange_worker, range(8)))
    final = Client(port)
    try:
        last = final.call('GET', 'exchange-race')
    finally:
        final.close()
    old_values = [old for old_list, _ in exchanges for old in old_list]
    new_values = [new for _, new_list in exchanges for new in new_list]
    assert len(set(old_values)) == 200
    assert set(old_values) == ({b'initial'} | set(new_values)) - {last}
    count += 203
    c = Client(port)
    try:
        assert c.call('GET', 'after-quit') is None
        fields = c.call('HGETALL', b'shared-hash\0')
        assert len(fields) == 800 and len(set(fields[::2])) == 400
        assert all(value.startswith(bytes(range(256))) for value in fields[1::2])
        count += 2
    finally:
        c.close()
    return count


def run_lists(port):
    c = Client(port)
    count = 0
    def check(expected, *args):
        nonlocal count
        actual = c.call(*args)
        assert type(actual) is type(expected) and actual == expected, (args, actual, expected)
        count += 1
    def error(prefix, *args):
        nonlocal count
        actual = c.call(*args)
        assert isinstance(actual, ErrorReply) and actual.startswith(prefix), (args, actual)
        count += 1
    key = b'list\0' + bytes(range(256))
    payload = [b'', bytes(range(256)), b'a\0b', b'last']
    deadline = 4102444800123
    try:
        pipeline_key = key + b':pipeline'
        requests = [('rpush', pipeline_key, str(i)) for i in range(65)]
        requests += [('LPOP', pipeline_key), ('RPOP', pipeline_key),
                     ('RPUSH', key, b'other'), ('LLEN', pipeline_key),
                     ('RPOP', key), ('DEL', pipeline_key)]
        c.socket.sendall(b''.join(c.encode(args) for args in requests))
        expected = list(range(1, 66)) + [b'0', b'64', 1, 63, b'other', 1]
        assert [c.read() for _ in requests] == expected
        count += len(requests)
        requests = [('RPUSH', pipeline_key, b'last'), ('RPOP', pipeline_key),
                    ('RPOP', pipeline_key), ('SADD', pipeline_key, b'member'),
                    ('RPUSH', pipeline_key, b'wrong-type'),
                    ('SADD', pipeline_key, b'member'), ('TYPE', pipeline_key),
                    ('DEL', pipeline_key)]
        c.socket.sendall(b''.join(c.encode(args) for args in requests))
        replies = [c.read() for _ in requests]
        assert replies[:4] == [1, b'last', None, 1]
        assert isinstance(replies[4], ErrorReply) and replies[4].startswith(b'WRONGTYPE ')
        assert replies[5:] == [0, b'set', 1]
        count += len(requests)
        model = [b'old'] * 128
        check(len(model), 'RPUSH', pipeline_key, *model)
        check(1, 'PEXPIREAT', pipeline_key, deadline)
        requests, expected = [], []
        for i in range(12):
            value = bytes([i, 0, 255])
            requests.extend([('RPUSH', pipeline_key, value),
                ('RPUSH', pipeline_key, b'temp'), ('RPOP', pipeline_key),
                ('LSET', pipeline_key, -1, value + b'!'),
                ('RPUSH', pipeline_key, value), ('LPUSH', pipeline_key, value),
                ('LPOP', pipeline_key), ('RPUSH', pipeline_key, b'end')])
            n = len(model)
            expected.extend([n + 1, n + 2, b'temp', b'OK', n + 2, n + 3, value, n + 3])
            model.extend([value + b'!', value, b'end'])
        c.socket.sendall(b''.join(c.encode(args) for args in requests))
        assert [c.read() for _ in requests] == expected
        count += len(requests)
        check(model, 'LRANGE', pipeline_key, 0, -1)
        check(deadline, 'PEXPIRETIME', pipeline_key)
        check(1, 'DEL', pipeline_key)
        mixed = [bytes([i, 0]).ljust(width, b'x')
                 for i, width in enumerate((512,) * 40 + (72, 255, 256) * 24)]
        check(1, 'RPUSH', pipeline_key, b'initial')
        check(1, 'PEXPIREAT', pipeline_key, deadline)
        requests = [('RPUSH', pipeline_key, value) for value in mixed]
        requests += [('LPUSH', pipeline_key, mixed[-1]), ('LPOP', pipeline_key),
                     ('RPOP', pipeline_key)]
        c.socket.sendall(b''.join(c.encode(args) for args in requests))
        expected = list(range(2, len(mixed) + 2)) + [len(mixed) + 2, mixed[-1], mixed[-1]]
        assert [c.read() for _ in requests] == expected
        count += len(requests)
        check([b'initial'] + mixed[:-1], 'LRANGE', pipeline_key, 0, -1)
        check(deadline, 'PEXPIRETIME', pipeline_key)
        check(1, 'DEL', pipeline_key)
        wide = [str(i).encode().ljust(1024, b'x') for i in range(80)]
        requests = [('LPUSH', pipeline_key, value) for value in wide]
        requests += [('RPUSH', pipeline_key, b'right'), ('RPOP', pipeline_key)]
        c.socket.sendall(b''.join(c.encode(args) for args in requests))
        assert [c.read() for _ in requests] == list(range(1, 81)) + [81, b'right']
        count += len(requests)
        requests = [('RPOP', pipeline_key)] * len(wide)
        requests += [('RPUSH', pipeline_key, b'new'), ('RPOP', pipeline_key)]
        c.socket.sendall(b''.join(c.encode(args) for args in requests))
        assert [c.read() for _ in requests] == wide + [1, b'new']
        count += len(requests)
        for width in (128, 1024):
            a, b = b'a' * width, b'b' * width
            requests = [('RPUSH', pipeline_key, a), ('RPUSH', pipeline_key, b),
                        ('RPOP', pipeline_key), ('RPOP', pipeline_key),
                        ('RPUSH', pipeline_key, b), ('LPUSH', pipeline_key, a),
                        ('RPOP', pipeline_key), ('RPOP', pipeline_key)]
            c.socket.sendall(b''.join(c.encode(args) for args in requests))
            assert [c.read() for _ in requests] == [1, 2, b, a, 1, 2, b, a]
            count += len(requests)
        batch = [bytes([i, 0, 255]) * 43 for i in range(64)]
        check(64, 'RPUSH', pipeline_key, *batch)
        requests = [('RPOP', pipeline_key, 32), ('LPOP', pipeline_key, 32),
                    ('RPOP', pipeline_key, 32), ('LPUSH', pipeline_key, b'new'),
                    ('RPOP', pipeline_key, 32)]
        c.socket.sendall(b''.join(c.encode(args) for args in requests))
        assert [c.read() for _ in requests] == [batch[32:][::-1],
                                               batch[:32], None, 1, [b'new']]
        count += len(requests)
        check(0, 'LLEN', key)
        check([], 'LRANGE', key, 0, -1)
        check(None, 'LINDEX', key, 'invalid')  # lookup precedes index parsing
        check(None, 'LPOP', key)
        check(None, 'RPOP', key, 0)
        check(b'OK', 'LTRIM', key, 0, -1)
        error(b'ERR no such key', 'LSET', key, 'invalid', b'x')
        check(0, 'LPUSHX', key, 'ignored')
        check(0, 'RPUSHX', key, 'ignored')
        check(0, 'EXISTS', key)
        check(4, 'RPUSH', key, *payload)
        check(b'list', 'TYPE', key)
        check([None, None], 'MGET', key, key)
        error(b'WRONGTYPE ', 'GET', key)
        error(b'WRONGTYPE ', 'HGET', key, 'field')
        check(4, 'LLEN', key)
        check(payload, 'LRANGE', key, 0, -1)
        for start in (-9223372036854775808, -9, -4, -2, -1, 0, 1, 3, 4, 9, 9223372036854775807):
            for end in (-9223372036854775808, -9, -4, -2, -1, 0, 1, 3, 4, 9, 9223372036854775807):
                first = start + len(payload) if start < 0 else start
                last = end + len(payload) if end < 0 else end
                first, last = max(first, 0), min(last, len(payload) - 1)
                expected = payload[first:last + 1] if first <= last else []
                check(expected, 'LRANGE', key, start, end)
        for index in (-9223372036854775808, -5, -4, -1, 0, 1, 3, 4, 9223372036854775807):
            expected = payload[index] if -len(payload) <= index < len(payload) else None
            check(expected, 'LINDEX', key, index)
        check(1, 'PEXPIREAT', key, deadline)
        check(b'OK', 'LSET', key, -1, b'changed\0')
        check(deadline, 'PEXPIRETIME', key)
        check(b'changed\0', 'LINDEX', key, -1)
        for bad in (b'', b'01', b'-0', b'+1', b'1\0', b'9223372036854775808'):
            error(b'ERR value is not an integer or out of range', 'LINDEX', key, bad)
            error(b'ERR value is not an integer or out of range', 'LSET', key, bad, 'bad')
            error(b'ERR value is not an integer or out of range', 'LRANGE', key, bad, -1)
            error(b'ERR value is not an integer or out of range', 'LTRIM', key, 0, bad)
            error(b'ERR value is out of range, must be positive', 'LPOP', key, bad)
            check(deadline, 'PEXPIRETIME', key)
        for index in (-5, 4):
            error(b'ERR index out of range', 'LSET', key, index, 'bad')
            check(b'changed\0', 'LINDEX', key, -1)
        check(6, 'LPUSH', key, b'left-one', b'left-two')
        check(8, 'RPUSHX', key, b'right-one', b'right-two')
        check(10, 'LPUSHX', key, b'x-one', b'x-two')
        check(deadline, 'PEXPIRETIME', key)
        check([], 'LPOP', key, 0)
        check(10, 'LLEN', key)
        check(b'x-two', 'LPOP', key)
        check([b'right-two', b'right-one'], 'RPOP', key, 2)
        check([b'x-one', b'left-two'], 'LPOP', key, 2)
        check(deadline, 'PEXPIRETIME', key)
        check(b'OK', 'LTRIM', key, 1, -2)
        check(payload[:3], 'LRANGE', key, 0, -1)
        check(deadline, 'PEXPIRETIME', key)
        check(list(reversed(payload[:3])), 'RPOP', key, 9223372036854775807)
        check(b'none', 'TYPE', key)
        # RESP2 distinguishes nil arrays from nil bulk strings.
        for args, wire in [(('LPOP', key), b'$-1\r\n'), (('LPOP', key, 0), b'*-1\r\n'),
                           (('RPOP', key, 1), b'*-1\r\n')]:
            c.socket.sendall(c.encode(args))
            assert c.stream.readline() == wire, args
            count += 1
        check(1, 'RPUSH', key, 'expire')
        check(1, 'PEXPIREAT', key, 1)
        check(0, 'LPUSHX', key, 'no-revival')
        check(0, 'EXISTS', key)
        check(1, 'LPUSH', key, 'revived')
        check(-1, 'PTTL', key)
        check(1, 'EXPIRE', key, 60)
        check(1, 'PERSIST', key)
        check([b'revived'], 'LRANGE', key, 0, -1)
        check(-1, 'PTTL', key)
        check(b'OK', 'LTRIM', key, -1, -2)
        check(0, 'EXISTS', key)
        check(b'OK', 'SET', 'list-wrong-type', 'string')
        cases = [('LPUSH', ('v',)), ('RPUSH', ('v',)), ('LPUSHX', ('v',)), ('RPUSHX', ('v',)),
                 ('LPOP', ()), ('RPOP', ()), ('LLEN', ()), ('LRANGE', (0, -1)),
                 ('LINDEX', ('invalid',)), ('LSET', ('invalid', 'v')), ('LTRIM', (0, -1))]
        for command, args in cases:
            error(b'WRONGTYPE ', command, 'list-wrong-type', *args)
            error(b'ERR wrong number of arguments', command)
        for command, args in [('LPOP', (1,)), ('RPOP', (1,)), ('LLEN', ()), ('LINDEX', (0,)),
                              ('LRANGE', (0, -1)), ('LSET', (0, 'v')), ('LTRIM', (0, -1))]:
            error(b'ERR wrong number of arguments', command, key, *args, 'extra')
        error(b'ERR value is not an integer or out of range', 'LRANGE', 'list-wrong-type', 'bad', -1)
        error(b'ERR value is not an integer or out of range', 'LTRIM', 'list-wrong-type', 0, 'bad')
        error(b'ERR value is out of range, must be positive', 'LPOP', 'list-wrong-type', -1)
        error(b'ERR value is out of range, must be positive', 'RPOP', key, -1)
        error(b'WRONGTYPE ', 'LPOP', 'list-wrong-type', 0)
        # Stateful model checks both directions and key removal after every step.
        rng = random.Random(451)
        model = []
        model_key = 'list-model'
        for step in range(300):
            operation = rng.choice(('LPUSH', 'RPUSH', 'LPUSHX', 'RPUSHX', 'LPOP', 'RPOP', 'LTRIM', 'LSET'))
            if operation in ('LPUSH', 'RPUSH', 'LPUSHX', 'RPUSHX'):
                values = [bytes([rng.randrange(256)]) + str(step).encode(), b'']
                if model or not operation.endswith('X'):
                    model = list(reversed(values)) + model if operation.startswith('L') else model + values
                check(len(model), operation, model_key, *values)
            elif operation in ('LPOP', 'RPOP'):
                take = rng.randrange(5)
                if not model: expected = None
                elif operation == 'LPOP':
                    expected, model = model[:take], model[take:]
                elif take:
                    expected, model = list(reversed(model[-take:])), model[:-take]
                else: expected = []
                check(expected, operation, model_key, take)
            elif operation == 'LTRIM':
                first, last = rng.randrange(-8, 8), rng.randrange(-8, 8)
                begin = max(0, first + len(model) if first < 0 else first)
                end = min(len(model) - 1, last + len(model) if last < 0 else last)
                model = model[begin:end + 1] if begin <= end else []
                check(b'OK', operation, model_key, first, last)
            elif model:
                position = rng.randrange(-len(model), len(model))
                model[position] = b'replaced\0'
                check(b'OK', 'LSET', model_key, position, b'replaced\0')
            else: error(b'ERR no such key', 'LSET', model_key, 0, 'v')
            check(model, 'LRANGE', model_key, 0, -1)
            check(len(model), 'LLEN', model_key)
            check(b'list' if model else b'none', 'TYPE', model_key)
    finally:
        c.close()
    start = threading.Barrier(8)
    def producer(number):
        client = Client(port)
        try:
            start.wait(timeout=10)
            blocks = []
            for i in range(40):
                values = [('list:%d:%d:%d' % (number, i, part)).encode() + b'\0' for part in range(2)]
                length = client.call('RPUSH', 'list-concurrent', *values)
                assert isinstance(length, int) and 2 <= length <= 640
                blocks.append((length, values))
            return blocks
        finally: client.close()
    with ThreadPoolExecutor(max_workers=8) as pool:
        blocks = [block for output in pool.map(producer, range(8)) for block in output]
    client = Client(port)
    try: all_values = client.call('LRANGE', 'list-concurrent', 0, -1)
    finally: client.close()
    assert len(all_values) == 640 and len({length for length, _ in blocks}) == 320
    assert all(all_values[length - 2:length] == values for length, values in blocks)
    count += 323
    start = threading.Barrier(8)
    def consumer(number):
        client = Client(port)
        try:
            start.wait(timeout=10)
            values, checks = [], 0
            while True:
                reply = client.call('LPOP' if number % 2 == 0 else 'RPOP', 'list-concurrent', 3)
                checks += 1
                if reply is None: break
                assert isinstance(reply, list) and 1 <= len(reply) <= 3, reply
                values.extend(reply)
            return values, checks
        finally: client.close()
    with ThreadPoolExecutor(max_workers=8) as pool:
        outcomes = list(pool.map(consumer, range(8)))
    removed = [value for values, _ in outcomes for value in values]
    assert len(removed) == len(set(removed)) == 640 and set(removed) == set(all_values)
    count += sum(checks for _, checks in outcomes) + 1
    return count


def run_list_transactions(port):
    count = 0
    c = Client(port)
    def check(expected, *args):
        nonlocal count
        actual = c.call(*args)
        assert type(actual) is type(expected) and actual == expected, (args, actual, expected)
        count += 1
    def error(prefix, *args):
        nonlocal count
        actual = c.call(*args)
        assert isinstance(actual, ErrorReply) and actual.startswith(prefix), (args, actual)
        count += 1
    src, dst = b'move-src\0' + bytes(range(256)), b'move-dst\0'
    try:
        check(b'OK', 'SET', dst, 'string')
        check(None, 'LMOVE', src, dst, 'LEFT', 'RIGHT')
        check(None, 'RPOPLPUSH', src, dst)
        check(b'string', 'GET', dst)
        for first in ('LEFT', 'RIGHT'):
            for second in ('LEFT', 'RIGHT'):
                check(1, 'DEL', dst, src)
                check(3, 'RPUSH', src, b'', bytes(range(256)), b'last\0')
                check(2, 'RPUSH', dst, b'dst-first', b'dst-last')
                check(1, 'PEXPIREAT', src, 4102444800123)
                check(1, 'PEXPIREAT', dst, 4102444900456)
                moved = b'' if first == 'LEFT' else b'last\0'
                check(moved, 'LMOVE', src, dst, first.lower(), second.lower())
                check([bytes(range(256)), b'last\0'] if first == 'LEFT' else [b'', bytes(range(256))], 'LRANGE', src, 0, -1)
                target = [b'dst-first', b'dst-last']
                check([moved] + target if second == 'LEFT' else target + [moved], 'LRANGE', dst, 0, -1)
                check(4102444800123, 'PEXPIRETIME', src)
                check(4102444900456, 'PEXPIRETIME', dst)
                check(2, 'DEL', src, dst)
                check(b'OK', 'SET', dst, 'string')
        check(1, 'DEL', dst)
        check(1, 'RPUSH', src, b'')
        check(1, 'PEXPIREAT', src, 4102444800123)
        check(b'', 'RPOPLPUSH', src, dst)
        check(b'none', 'TYPE', src)
        check([b''], 'LRANGE', dst, 0, -1)
        check(-1, 'PTTL', dst)
        check(1, 'DEL', dst)
        check(3, 'RPUSH', src, 'a', 'b', 'c')
        check(1, 'PEXPIREAT', src, 4102444800123)
        model = [b'a', b'b', b'c']
        for first, second in [('LEFT', 'LEFT'), ('RIGHT', 'RIGHT'), ('LEFT', 'RIGHT'), ('RIGHT', 'LEFT')]:
            item = model.pop(0 if first == 'LEFT' else -1)
            model.insert(0, item) if second == 'LEFT' else model.append(item)
            check(item, 'LMOVE', src, src, first, second)
            check(model, 'LRANGE', src, 0, -1)
            check(4102444800123, 'PEXPIRETIME', src)
        check(b'c', 'RPOPLPUSH', src, src)
        check([b'c', b'a', b'b'], 'LRANGE', src, 0, -1)
        check(b'OK', 'SET', dst, 'wrong')
        error(b'WRONGTYPE ', 'LMOVE', src, dst, 'RIGHT', 'LEFT')
        check([b'c', b'a', b'b'], 'LRANGE', src, 0, -1)
        check(4102444800123, 'PEXPIRETIME', src)
        check(b'wrong', 'GET', dst)
        error(b'WRONGTYPE ', 'RPOPLPUSH', dst, src)
        for command in [('LMOVE', src, dst, 'UP', 'LEFT'), ('LMOVE', src, dst, 'LEFT', ''),
                        ('LMOVE', 'missing-move', dst, 'bad', 'bad')]:
            error(b'ERR syntax error', *command)
        for command in [('LMOVE',), ('LMOVE', src, dst, 'LEFT'), ('LMOVE', src, dst, 'LEFT', 'LEFT', 'extra'),
                        ('RPOPLPUSH',), ('RPOPLPUSH', src, dst, 'extra'), ('LMPOP',), ('LMPOP', 1, src)]:
            error(b'ERR wrong number of arguments', *command)
        check(1, 'PEXPIREAT', dst, 1)
        check(b'b', 'LMOVE', src, dst, 'RIGHT', 'RIGHT')
        check([b'b'], 'LRANGE', dst, 0, -1)
        check(-1, 'PTTL', dst)
        check(1, 'PEXPIREAT', src, 1)
        check(None, 'RPOPLPUSH', src, dst)
        check([b'b'], 'LRANGE', dst, 0, -1)
        # LMPOP chooses by request order, ignores later wrong types, preserves TTL.
        check(3, 'RPUSH', src, 'one', b'', 'three')
        check(1, 'PEXPIREAT', src, 4102444800123)
        check(b'OK', 'SET', 'mpop-wrong', 'string')
        check([src, [b'three', b'']], 'LMPOP', 4, 'mpop-missing', src, src, 'mpop-wrong', 'RIGHT', 'COUNT', 2)
        check(4102444800123, 'PEXPIRETIME', src)
        error(b'WRONGTYPE ', 'LMPOP', 2, 'mpop-wrong', src, 'LEFT')
        check([b'one'], 'LRANGE', src, 0, -1)
        check([src, [b'one']], 'LMPOP', 2, src, 'mpop-wrong', 'LEFT', 'COUNT', 9223372036854775807)
        check(b'none', 'TYPE', src)
        check(None, 'LMPOP', 3, src, 'mpop-missing', src, 'LEFT')
        c.socket.sendall(c.encode(('LMPOP', 1, src, 'LEFT')))
        assert c.stream.readline() == b'*-1\r\n'
        count += 1
        for bad in (0, -1, b'', b'01', b'+1', b'-0', b'1\0', b'9223372036854775808'):
            error(b'ERR numkeys should be greater than 0', 'LMPOP', bad, src, 'LEFT')
            error(b'ERR count should be greater than 0', 'LMPOP', 1, src, 'LEFT', 'COUNT', bad)
            error(b'ERR count should be greater than 0', 'LMPOP', 1, src, 'LEFT', 'COUNT', bad, 'trailing')
        for command in [('LMPOP', 9223372036854775807, src, 'LEFT'), ('LMPOP', 2, src, 'LEFT'),
                        ('LMPOP', 1, src, 'bad'), ('LMPOP', 1, src, 'LEFT', 'COUNT'),
                        ('LMPOP', 1, src, 'LEFT', 'other', 1), ('LMPOP', 1, src, 'LEFT', 'COUNT', 1, 'COUNT', 2)]:
            error(b'ERR syntax error', *command)
        # Model same-key rotations, both directions, destination creation/deletion.
        rng = random.Random(16449)
        names = [b'move-model-a', b'move-model-b', b'move-model-c']
        models = {name: [] for name in names}
        for step in range(240):
            source, destination = rng.choice(names), rng.choice(names)
            if rng.randrange(4) == 0:
                token = str(step).encode() + b'\0'
                models[source].append(token)
                check(len(models[source]), 'RPUSH', source, token)
            elif rng.randrange(3) == 0:
                order = [rng.choice(names) for _ in range(4)]
                left, take = rng.choice([True, False]), rng.randrange(1, 5)
                expected = None
                for name in order:
                    values = models[name]
                    if not values: continue
                    if left: picked, models[name] = values[:take], values[take:]
                    else: picked, models[name] = list(reversed(values[-take:])), values[:-take]
                    expected = [name, picked]
                    break
                check(expected, 'LMPOP', len(order), *order, 'LEFT' if left else 'RIGHT', 'COUNT', take)
            else:
                first, second = rng.choice(['LEFT', 'RIGHT']), rng.choice(['LEFT', 'RIGHT'])
                if not models[source]: item = None
                else:
                    item = models[source].pop(0 if first == 'LEFT' else -1)
                    models[destination].insert(0, item) if second == 'LEFT' else models[destination].append(item)
                check(item, 'LMOVE', source, destination, first, second)
            for name in names:
                check(models[name], 'LRANGE', name, 0, -1)
                check(b'list' if models[name] else b'none', 'TYPE', name)
        # Drain a shared source into three targets; each value must move once.
        tokens = [('move-token:%d' % i).encode() + b'\0' for i in range(480)]
        check(480, 'RPUSH', 'move-race-source', *tokens)
        for name in ('mpop-race-a', 'mpop-race-b'):
            check(240, 'RPUSH', name, *(name.encode() + str(i).encode() for i in range(240)))
    finally: c.close()
    start = threading.Barrier(8)
    def worker(number):
        client = Client(port)
        try:
            start.wait(timeout=10)
            moved, popped, checks = [], [], 0
            while True:
                value = client.call('LMOVE', 'move-race-source', 'move-race-dst:%d' % (number % 3),
                                    'LEFT' if number % 2 else 'RIGHT', 'LEFT' if number % 3 else 'RIGHT')
                checks += 1
                if value is None: break
                assert isinstance(value, bytes)
                moved.append(value)
            while True:
                reply = client.call('LMPOP', 3, 'mpop-race-a', 'mpop-race-a', 'mpop-race-b',
                                    'LEFT' if number % 2 else 'RIGHT', 'COUNT', 3)
                checks += 1
                if reply is None: break
                assert len(reply) == 2 and reply[0] in (b'mpop-race-a', b'mpop-race-b')
                assert 1 <= len(reply[1]) <= 3 and all(value.startswith(reply[0]) for value in reply[1])
                popped.extend(reply[1])
            return moved, popped, checks
        finally: client.close()
    with ThreadPoolExecutor(max_workers=8) as pool: outcomes = list(pool.map(worker, range(8)))
    moved = [value for values, _, _ in outcomes for value in values]
    popped = [value for _, values, _ in outcomes for value in values]
    assert len(moved) == len(set(moved)) == 480 and set(moved) == set(tokens)
    expected = {name.encode() + str(i).encode() for name in ('mpop-race-a', 'mpop-race-b') for i in range(240)}
    assert len(popped) == len(set(popped)) == 480 and set(popped) == expected
    count += sum(checks for _, _, checks in outcomes) + 2
    start = threading.Barrier(8)
    def shuffle(number):
        client = Client(port)
        try:
            start.wait(timeout=10)
            for i in range(60):
                source = (number + i) % 3
                target = (source + (1 if number % 2 else 2)) % 3
                value = client.call('LMOVE', 'move-race-dst:%d' % source, 'move-race-dst:%d' % target, 'RIGHT', 'LEFT')
                assert value is None or value in tokens
            return 60
        finally: client.close()
    with ThreadPoolExecutor(max_workers=8) as pool: count += sum(pool.map(shuffle, range(8)))
    c = Client(port)
    try:
        all_targets = [value for number in range(3) for value in c.call('LRANGE', 'move-race-dst:%d' % number, 0, -1)]
        assert len(all_targets) == len(set(all_targets)) == 480 and set(all_targets) == set(tokens)
        count += 1
        check(0, 'EXISTS', 'move-race-source', 'mpop-race-a', 'mpop-race-b')
    finally: c.close()
    return count


def run_list_edit_search(port):
    count = 0
    c = Client(port)
    def check(expected, *args):
        nonlocal count
        actual = c.call(*args)
        assert type(actual) is type(expected) and actual == expected, (args, actual, expected)
        count += 1
    def error(prefix, *args):
        nonlocal count
        actual = c.call(*args)
        assert isinstance(actual, ErrorReply) and actual.startswith(prefix), (args, actual)
        count += 1
    def positions(values, value, rank, take, maxlen):
        order = list(range(len(values))) if rank > 0 else list(reversed(range(len(values))))
        if maxlen: order = order[:maxlen]
        matches = [index for index in order if values[index] == value][abs(rank) - 1:]
        return matches[:take] if take else matches
    key = b'list-edit-search\0'
    binary = bytes(range(256))
    values = [b'', binary, b'1', b'01', b'', binary, b'end', binary]
    deadline = 4102444800123
    try:
        check(0, 'LINSERT', key, 'BEFORE', 'pivot', 'value')
        check(0, 'LREM', key, 0, 'value')
        check(None, 'LPOS', key, 'value')
        check([], 'LPOS', key, 'value', 'COUNT', 0)
        error(b'ERR syntax error', 'LINSERT', key, 'bad', 'pivot', 'value')
        check(8, 'RPUSH', key, *values)
        check(1, 'PEXPIREAT', key, deadline)
        check(-1, 'LINSERT', key, 'AFTER', 'missing-pivot', 'value')
        check(values, 'LRANGE', key, 0, -1)
        check(deadline, 'PEXPIRETIME', key)
        for value in (b'', binary, b'1', b'01', b'missing'):
            check(next((i for i, item in enumerate(values) if item == value), None), 'LPOS', key, value)
            for rank in (-9223372036854775807, -4, -2, -1, 1, 2, 4, 9223372036854775807):
                for take in (0, 1, 2, 9223372036854775807):
                    for maxlen in (0, 1, 3, 8, 9, 9223372036854775807):
                        expected = positions(values, value, rank, take, maxlen)
                        check(expected, 'LPOS', key, value, 'RANK', rank, 'COUNT', take, 'MAXLEN', maxlen)
                expected = positions(values, value, rank, 1, 0)
                check(expected[0] if expected else None, 'LPOS', key, value, 'rank', rank)
        # Repeated options use the last argument; invalid earlier values still fail.
        check([5, 7], 'LPOS', key, binary, 'RANK', 1, 'RANK', 2, 'COUNT', 1, 'COUNT', 0)
        check([1], 'LPOS', key, binary, 'MAXLEN', 1, 'MAXLEN', 3, 'COUNT', 0)
        check(None, 'LPOS', key, binary, 'MAXLEN', 1)
        for name, message in [('COUNT', b"ERR COUNT can't be negative"), ('MAXLEN', b"ERR MAXLEN can't be negative")]:
            for bad in (-1, b'', b'01', b'-0', b'+1', b'1\0', b'9223372036854775808'):
                error(message, 'LPOS', key, binary, name, bad)
                error(message, 'LPOS', 'missing-search', binary, name, bad, name, 1)
        error(b"ERR RANK can't be zero", 'LPOS', key, binary, 'RANK', 0)
        error(b'ERR value is out of range, value must between', 'LPOS', key, binary, 'RANK', '-9223372036854775808')
        for bad in (b'', b'01', b'-0', b'+1', b'1\0', b'9223372036854775808'):
            error(b'ERR value is not an integer or out of range', 'LPOS', key, binary, 'RANK', bad)
            error(b'ERR value is not an integer or out of range', 'LREM', key, bad, binary)
        error(b'ERR value is out of range, value must between', 'LREM', key, '-9223372036854775808', binary)
        for args in [('RANK',), ('COUNT',), ('MAXLEN',), ('UNKNOWN',), ('UNKNOWN', 1), ('RANK', 1, 'UNKNOWN')]:
            error(b'ERR syntax error', 'LPOS', key, binary, *args)
        check(deadline, 'PEXPIRETIME', key)
        check(9, 'LINSERT', key, 'before', binary, b'new\0')
        values.insert(1, b'new\0')
        check(10, 'LINSERT', key, 'after', binary, b'after\0')
        values.insert(3, b'after\0')
        check(values, 'LRANGE', key, 0, -1)
        check(1, 'LREM', key, -1, binary)
        del values[len(values) - 1 - list(reversed(values)).index(binary)]
        check(values, 'LRANGE', key, 0, -1)
        check(1, 'LREM', key, 1, binary)
        values.remove(binary)
        check(values, 'LRANGE', key, 0, -1)
        check(1, 'LREM', key, 0, binary)
        values = [item for item in values if item != binary]
        check(values, 'LRANGE', key, 0, -1)
        check(0, 'LREM', key, 9223372036854775807, binary)
        check(0, 'LREM', key, -9223372036854775807, binary)
        check(deadline, 'PEXPIRETIME', key)
        check(b'OK', 'SET', 'list-search-wrong', 'string')
        for args in [('LINSERT', 'list-search-wrong', 'BEFORE', 'pivot', 'value'),
                     ('LREM', 'list-search-wrong', 0, 'value'), ('LPOS', 'list-search-wrong', 'value', 'COUNT', 0)]:
            error(b'WRONGTYPE ', *args)
        error(b'ERR syntax error', 'LINSERT', 'list-search-wrong', 'bad', 'pivot', 'value')
        error(b'ERR value is not an integer or out of range', 'LREM', 'list-search-wrong', 'bad', 'value')
        error(b"ERR RANK can't be zero", 'LPOS', 'list-search-wrong', 'value', 'RANK', 0)
        for command in [('LINSERT',), ('LINSERT', key, 'BEFORE', 'pivot'), ('LINSERT', key, 'BEFORE', 'pivot', 'v', 'extra'),
                        ('LREM',), ('LREM', key, 0, 'v', 'extra'), ('LPOS',), ('LPOS', key)]:
            error(b'ERR wrong number of arguments', *command)
        check(1, 'PEXPIREAT', key, 1)
        check(0, 'LINSERT', key, 'BEFORE', 'pivot', 'value')
        check(0, 'LREM', key, 0, 'value')
        check(None, 'LPOS', key, 'value')
        check([], 'LPOS', key, 'value', 'COUNT', 0)
        check(0, 'EXISTS', key)
        check(3, 'RPUSH', key, b'', b'', b'')
        check(3, 'LREM', key, 0, b'')
        check(b'none', 'TYPE', key)
        # Stateful model includes interleaved insertions, directional removals,
        # searches, duplicate pivots and length-zero key deletion.
        rng = random.Random(92741)
        model = []
        tokens = [b'', binary, b'a\0', b'1', b'01']
        for step in range(280):
            operation = rng.choice(('RPUSH', 'LINSERT', 'LREM', 'LPOS'))
            value, pivot = rng.choice(tokens), rng.choice(tokens)
            if operation == 'RPUSH':
                model.append(value)
                check(len(model), operation, key, value)
            elif operation == 'LINSERT':
                before = rng.choice([True, False])
                if not model: expected = 0
                elif pivot not in model: expected = -1
                else:
                    model.insert(model.index(pivot) + (0 if before else 1), value)
                    expected = len(model)
                check(expected, operation, key, 'BEFORE' if before else 'AFTER', pivot, value)
            elif operation == 'LREM':
                amount = rng.choice([0, 1, 2, -1, -2, -9223372036854775807, 9223372036854775807])
                indexes = [i for i, item in enumerate(model) if item == value]
                if amount < 0: indexes.reverse()
                if amount: indexes = indexes[:abs(amount)]
                removed = set(indexes)
                model = [item for i, item in enumerate(model) if i not in removed]
                check(len(removed), operation, key, amount, value)
            else:
                rank, take, maxlen = rng.choice([-3, -1, 1, 3]), rng.randrange(4), rng.randrange(8)
                check(positions(model, value, rank, take, maxlen), 'LPOS', key, value, 'RANK', rank, 'COUNT', take, 'MAXLEN', maxlen)
            check(model, 'LRANGE', key, 0, -1)
            check(len(model), 'LLEN', key)
            check(b'list' if model else b'none', 'TYPE', key)
        check(1, 'RPUSH', 'list-insert-race', 'anchor')
        check(256, 'RPUSH', 'list-remove-race', *([b'keep', b'remove'] * 128))
    finally: c.close()
    start = threading.Barrier(8)
    def worker(number):
        client = Client(port)
        try:
            start.wait(timeout=10)
            inserted, removed = [], 0
            for i in range(30):
                token = ('insert:%d:%d' % (number, i)).encode() + b'\0'
                length = client.call('LINSERT', 'list-insert-race', 'BEFORE', 'anchor', token)
                assert isinstance(length, int) and 2 <= length <= 241
                inserted.append((length, token))
                value = client.call('LREM', 'list-remove-race', -1 if number % 2 else 1, b'remove')
                assert value in (0, 1)
                removed += value
            return inserted, removed
        finally: client.close()
    with ThreadPoolExecutor(max_workers=8) as pool: outcomes = list(pool.map(worker, range(8)))
    c = Client(port)
    try:
        items = c.call('LRANGE', 'list-insert-race', 0, -1)
        blocks = [item for inserted, _ in outcomes for item in inserted]
        assert len(items) == 241 and items[-1] == b'anchor' and len({length for length, _ in blocks}) == 240
        assert all(items[length - 2] == token for length, token in blocks)
        assert sum(removed for _, removed in outcomes) == 128
        check([b'keep'] * 128, 'LRANGE', 'list-remove-race', 0, -1)
        count += 484
    finally: c.close()
    return count


def run_sets(port):
    c = Client(port)
    count = 0
    def check(expected, *args):
        nonlocal count
        actual = c.call(*args)
        assert type(actual) is type(expected) and actual == expected, (args, actual, expected)
        count += 1
    def check_set(expected, *args):
        nonlocal count
        actual = c.call(*args)
        assert isinstance(actual, list) and len(actual) == len(set(actual)) and set(actual) == expected, (args, actual, expected)
        count += 1
    def error(prefix, *args):
        nonlocal count
        actual = c.call(*args)
        assert isinstance(actual, ErrorReply) and actual.startswith(prefix), (args, actual)
        count += 1
    key, target = b'set-basic\0', b'set-target\0'
    binary = bytes(range(256))
    deadline = 4102444800123
    try:
        pipeline_key = key + b':pipeline'
        requests = [('sadd', pipeline_key, binary)] * 17
        requests += [('SREM', pipeline_key, binary), ('SADD', pipeline_key, b''),
                     ('SCARD', pipeline_key), ('SET', pipeline_key, 'string'),
                     ('SADD', pipeline_key, 'a'), ('SADD', pipeline_key, 'b'),
                     ('GET', pipeline_key), ('DEL', pipeline_key)]
        c.socket.sendall(b''.join(c.encode(args) for args in requests))
        replies = [c.read() for _ in requests]
        assert replies[:21] == [1] + [0] * 16 + [1, 1, 1, b'OK']
        assert all(isinstance(reply, ErrorReply) and reply.startswith(b'WRONGTYPE ')
                   for reply in replies[21:23])
        assert replies[23:] == [b'string', 1]
        count += len(requests)
        requests = [('SPOP', pipeline_key), ('spop', pipeline_key),
                    ('SADD', pipeline_key, binary), ('SPOP', pipeline_key),
                    ('SPOP', pipeline_key), ('SADD', pipeline_key, b''),
                    ('SPOP', pipeline_key), ('SPOP', pipeline_key)]
        c.socket.sendall(b''.join(c.encode(args) for args in requests))
        assert [c.read() for _ in requests] == [
            None, None, 1, binary, None, 1, b'', None]
        count += len(requests)
        bucketed = key + b':bucketed-pipeline'
        members = [b'member\0%04d' % i for i in range(4096)]
        check(len(members), 'SADD', bucketed, *members)
        requests = [('SADD', bucketed, member) for member in members[::128]]
        requests.extend([('SADD', bucketed, binary), ('SADD', bucketed, binary)])
        c.socket.sendall(b''.join(c.encode(args) for args in requests))
        assert [c.read() for _ in requests] == [0] * 32 + [1, 0]
        count += len(requests)
        check(len(members) + 1, 'SCARD', bucketed)
        for size in (6, 7, 32, 33):
            absent = b'absent\0member'
            args = [binary, *members[:size - 3], absent, binary]
            query = ('SMISMEMBER', bucketed, *args)
            varied = ('SMISMEMBER', bucketed, absent, *args[1:])
            present = [1] * (size - 2) + [0, 1]
            removed = [0, *present[1:-1], 0]
            requests = [query, query, varied, ('SREM', bucketed, binary),
                        query, query, ('SADD', bucketed, binary), query]
            c.socket.sendall(b''.join(c.encode(command) for command in requests))
            assert [c.read() for _ in requests] == [
                present, present, [0, *present[1:]], 1, removed, removed, 1, present]
            count += len(requests)
        check(1, 'DEL', bucketed)
        plain = key + b':membership-tree'
        prefix = b'fragment\0' + b'x' * 72
        long_members = [prefix + bytes([i]) for i in range(4)]
        plain_members = members[:188] + long_members
        check(len(plain_members), 'SADD', plain, *plain_members)
        probe = [long_members[3], prefix, members[7], long_members[0],
                 long_members[3], b'absent\0member', members[0]]
        expected = [1, 0, 1, 1, 1, 0, 1]
        check(expected, 'SMISMEMBER', plain, *probe)
        check(expected * 6, 'SMISMEMBER', plain, *(probe * 6))
        check(1, 'DEL', plain)
        check(0, 'SCARD', key)
        check(0, 'SISMEMBER', key, b'')
        check([0, 0, 0], 'SMISMEMBER', key, b'', 'a', b'')
        check_set(set(), 'SMEMBERS', key)
        check(0, 'SREM', key, 'missing')
        check(5, 'SADD', key, b'', binary, b'a\0b', b'1', b'01', binary, b'')
        check(b'set', 'TYPE', key)
        check(5, 'SCARD', key)
        check(0, 'SADD', key, binary, b'')
        check(1, 'SISMEMBER', key, binary)
        check([1, 0, 1, 1, 1, 1], 'SMISMEMBER', key, binary, 'missing', b'', binary, '1', '01')
        check_set({b'', binary, b'a\0b', b'1', b'01'}, 'SMEMBERS', key)
        check(1, 'PEXPIREAT', key, deadline)
        check(1, 'SADD', key, 'new')
        check(1, 'SREM', key, 'new', 'new', 'missing')
        check(deadline, 'PEXPIRETIME', key)
        error(b'WRONGTYPE ', 'GET', key)
        check([None], 'MGET', key)
        error(b'WRONGTYPE ', 'HGET', key, 'field')
        error(b'WRONGTYPE ', 'LLEN', key)
        check(b'OK', 'SET', target, 'string')
        check(0, 'SMOVE', 'set-no-source', target, 'member')
        error(b'WRONGTYPE ', 'SMOVE', key, target, 'missing-member')
        error(b'WRONGTYPE ', 'SMOVE', target, key, binary)
        check_set({b'', binary, b'a\0b', b'1', b'01'}, 'SMEMBERS', key)
        check(deadline, 'PEXPIRETIME', key)
        check(1, 'SMOVE', key, key, binary)
        check(0, 'SMOVE', key, key, 'missing')
        check(deadline, 'PEXPIRETIME', key)
        check(1, 'DEL', target)
        check(2, 'SADD', target, 'target', binary)
        check(1, 'PEXPIREAT', target, deadline + 555)
        check(1, 'SMOVE', key, target, binary)  # destination already has it
        check(deadline + 555, 'PEXPIRETIME', target)
        check(1, 'SMOVE', key, target, b'')
        check(0, 'SMOVE', key, target, b'')
        check(deadline, 'PEXPIRETIME', key)
        check_set({b'target', binary, b''}, 'SMEMBERS', target)
        check(1, 'PEXPIREAT', target, 1)
        check(1, 'SMOVE', key, target, b'01')
        check(-1, 'PTTL', target)
        check_set({b'01'}, 'SMEMBERS', target)
        check(2, 'SREM', key, b'1', b'a\0b')
        check(b'none', 'TYPE', key)
        check(1, 'SADD', key, 'expired')
        check(1, 'PEXPIREAT', key, 1)
        check(0, 'SREM', key, 'expired')
        check(1, 'SADD', key, 'revived')
        check(-1, 'PTTL', key)
        check(1, 'EXPIRE', key, 60)
        check(1, 'PERSIST', key)
        check_set({b'revived'}, 'SMEMBERS', key)
        a, b, d = b'set-algebra-a', b'set-algebra-b', b'set-algebra-destination'
        check(3, 'SADD', a, b'', binary, 'a')
        check(3, 'SADD', b, b'', binary, 'b')
        operations = [('SUNION', {b'', binary, b'a', b'b'}), ('SINTER', {b'', binary}), ('SDIFF', {b'a'})]
        for command, expected in operations:
            check_set(expected, command, a, b)
            check_set({b'', binary, b'a'} if command != 'SDIFF' else set(), command, a, a)
            check_set(set() if command != 'SUNION' else {b'', binary, b'a'}, command, 'set-algebra-missing', a)
            check(b'OK', 'SET', d, 'previous-string', 'PXAT', deadline)
            check(len(expected), command + 'STORE', d, a, b)
            check_set(expected, 'SMEMBERS', d)
            check(-1, 'PTTL', d)
        check(1, 'PEXPIREAT', a, deadline)
        check(1, 'PEXPIREAT', b, deadline + 555)
        check(2, 'SINTERSTORE', a, a, b)
        check_set({b'', binary}, 'SMEMBERS', a)
        check(-1, 'PTTL', a)
        check(deadline + 555, 'PEXPIRETIME', b)
        check(0, 'SDIFFSTORE', a, a, a)
        check(b'none', 'TYPE', a)
        check(b'OK', 'SET', d, 'delete-on-empty')
        check(0, 'SINTERSTORE', d, a, b)
        check(b'none', 'TYPE', d)
        check(b'OK', 'SET', 'set-wrong', 'string')
        for command in ('SUNION', 'SINTER', 'SDIFF'):
            error(b'WRONGTYPE ', command, 'set-algebra-missing', 'set-wrong')
            check(b'OK', 'SET', d, 'preserved', 'PXAT', deadline)
            error(b'WRONGTYPE ', command + 'STORE', d, 'set-algebra-missing', 'set-wrong')
            check(b'preserved', 'GET', d)
            check(deadline, 'PEXPIRETIME', d)
        for command, args in [('SADD', ('member',)), ('SREM', ('member',)), ('SCARD', ()),
                              ('SISMEMBER', ('member',)), ('SMISMEMBER', ('member',)), ('SMEMBERS', ())]:
            error(b'WRONGTYPE ', command, 'set-wrong', *args)
        for command in ('SADD', 'SREM', 'SCARD', 'SISMEMBER', 'SMISMEMBER', 'SMEMBERS', 'SMOVE',
                        'SUNION', 'SINTER', 'SDIFF', 'SUNIONSTORE', 'SINTERSTORE', 'SDIFFSTORE'):
            error(b'ERR wrong number of arguments', command)
        for command, args in [('SCARD', ()), ('SMEMBERS', ()), ('SISMEMBER', ('v',)), ('SMOVE', (key, 'v'))]:
            error(b'ERR wrong number of arguments', command, key, *args, 'extra')
        # All operations share one model, including destination/source aliases.
        names = [b'set-model-a', b'set-model-b', b'set-model-c']
        models = {name: set() for name in names}
        tokens = [b'', binary, b'a\0', b'1', b'01', b'x', b'y']
        rng = random.Random(44282)
        for step in range(280):
            command = rng.choice(('SADD', 'SREM', 'SMOVE', 'SUNION', 'SINTER', 'SDIFF', 'SUNIONSTORE', 'SINTERSTORE', 'SDIFFSTORE'))
            source, destination, member = rng.choice(names), rng.choice(names), rng.choice(tokens)
            if command in ('SADD', 'SREM'):
                request = [member, member, rng.choice(tokens)]
                before = len(models[source])
                if command == 'SADD': models[source].update(request)
                else: models[source].difference_update(request)
                check(abs(before - len(models[source])), command, source, *request)
            elif command == 'SMOVE':
                moved = int(member in models[source])
                if moved and source != destination:
                    models[source].remove(member)
                    models[destination].add(member)
                check(moved, command, source, destination, member)
            else:
                order = [rng.choice(names) for _ in range(3)]
                expected = set(models[order[0]])
                for name in order[1:]:
                    if command.startswith('SUNION'): expected.update(models[name])
                    elif command.startswith('SINTER'): expected.intersection_update(models[name])
                    else: expected.difference_update(models[name])
                if command.endswith('STORE'):
                    models[destination] = expected
                    check(len(expected), command, destination, *order)
                else: check_set(expected, command, *order)
            for name in names:
                check_set(models[name], 'SMEMBERS', name)
                check(b'set' if models[name] else b'none', 'TYPE', name)
        expected = {('set-thread:%d:%d' % (number, i)).encode() + b'\0' for number in range(8) for i in range(40)}
        shared = {('common:%d' % i).encode() for i in range(16)}
    finally: c.close()
    start = threading.Barrier(8)
    def writer(number):
        client = Client(port)
        added = 0
        try:
            start.wait(timeout=10)
            for i in range(40):
                member = ('set-thread:%d:%d' % (number, i)).encode() + b'\0'
                reply = client.call('SADD', 'set-concurrent', member, member, 'common:%d' % (i % 16))
                assert reply in (1, 2), reply
                added += reply
            return added
        finally: client.close()
    with ThreadPoolExecutor(max_workers=8) as pool: added = list(pool.map(writer, range(8)))
    assert sum(added) == 336
    count += 321
    start = threading.Barrier(8)
    def remover(number):
        client = Client(port)
        try:
            start.wait(timeout=10)
            reply = client.call('SREM', 'set-concurrent', *sorted(shared))
            assert isinstance(reply, int) and 0 <= reply <= 16
            return reply
        finally: client.close()
    with ThreadPoolExecutor(max_workers=8) as pool: removed = list(pool.map(remover, range(8)))
    assert sum(removed) == 16
    count += 9
    start = threading.Barrier(9)
    def mover(number):
        client = Client(port)
        try:
            start.wait(timeout=10)
            for i in range(40):
                member = ('set-thread:%d:%d' % (number, i)).encode() + b'\0'
                assert client.call('SMOVE', 'set-concurrent', 'set-concurrent-target', member) == 1
                assert client.call('SMOVE', 'set-concurrent-target', 'set-concurrent', member) == 1
            return 80
        finally: client.close()
    def observer(_):
        client = Client(port)
        try:
            start.wait(timeout=10)
            for _ in range(100):
                reply = client.call('SUNION', 'set-concurrent', 'set-concurrent-target')
                assert len(reply) == len(set(reply)) == 320 and set(reply) == expected
                assert client.call('SINTER', 'set-concurrent', 'set-concurrent-target') == []
            return 200
        finally: client.close()
    with ThreadPoolExecutor(max_workers=9) as pool:
        work = [pool.submit(mover, i) for i in range(8)] + [pool.submit(observer, 0)]
        count += sum(task.result() for task in work)
    c = Client(port)
    try:
        check_set(expected, 'SMEMBERS', 'set-concurrent')
        check(b'none', 'TYPE', 'set-concurrent-target')
    finally: c.close()
    return count


def run_set_sampling(port):
    c = Client(port)
    count = 0
    def check(expected, *args):
        nonlocal count
        actual = c.call(*args)
        assert type(actual) is type(expected) and actual == expected, (args, actual, expected)
        count += 1
    def members(expected, key):
        nonlocal count
        actual = c.call('SMEMBERS', key)
        assert isinstance(actual, list) and len(actual) == len(set(actual)) and set(actual) == expected
        count += 1
    def error(prefix, *args):
        nonlocal count
        actual = c.call(*args)
        assert isinstance(actual, ErrorReply) and actual.startswith(prefix), (args, actual)
        count += 1
    binary = bytes(range(256))
    values = {b'', binary, b'1', b'01'}
    deadline = 4102444800123
    try:
        check(0, 'SINTERCARD', 1, 'sample-missing')
        check(5, 'SADD', 'card-a', b'', binary, 'a', '1', '01')
        check(4, 'SADD', 'card-b', b'', binary, 'b', '1')
        check(3, 'SADD', 'card-c', b'', '1', 'c')
        check(3, 'SINTERCARD', 2, 'card-a', 'card-b')
        check(2, 'SINTERCARD', 3, 'card-a', 'card-b', 'card-c')
        check(3, 'SINTERCARD', 4, 'card-b', 'card-a', 'card-b', 'card-a')
        for limit in (0, 1, 2, 3, 4, 9223372036854775807):
            check(min(3, limit) if limit else 3, 'SINTERCARD', 2, 'card-a', 'card-b', 'LIMIT', limit)
        check(3, 'SINTERCARD', 2, 'card-a', 'card-b', 'LIMIT', 1, 'limit', 0)
        check(b'OK', 'SET', 'sample-wrong', 'string')
        error(b'WRONGTYPE ', 'SINTERCARD', 2, 'sample-missing', 'sample-wrong', 'LIMIT', 1)
        for bad in (0, -1, b'', b'01', b'+1', b'-0', b'1\0', b'9223372036854775808'):
            error(b'ERR numkeys should be greater than 0', 'SINTERCARD', bad, 'card-a')
        for bad in (-1, b'', b'01', b'+1', b'-0', b'1\0', b'9223372036854775808'):
            error(b"ERR LIMIT can't be negative", 'SINTERCARD', 1, 'card-a', 'LIMIT', bad)
            error(b"ERR LIMIT can't be negative", 'SINTERCARD', 1, 'card-a', 'LIMIT', bad, 'LIMIT', 0)
        error(b"ERR Number of keys can't be greater than number of args", 'SINTERCARD', 9223372036854775807, 'card-a')
        for args in [('LIMIT',), ('UNKNOWN',), ('UNKNOWN', 1), ('LIMIT', 1, 'UNKNOWN')]:
            error(b'ERR syntax error', 'SINTERCARD', 1, 'card-a', *args)
        check(1, 'PEXPIREAT', 'card-a', deadline)
        check(3, 'SINTERCARD', 2, 'card-a', 'card-b')
        check(deadline, 'PEXPIRETIME', 'card-a')
        check(1, 'PEXPIREAT', 'card-a', 1)
        check(0, 'SINTERCARD', 2, 'card-a', 'card-b')
        for command in ('SPOP', 'SRANDMEMBER'):
            check(None, command, 'sample-missing')
            for amount in (0, 1, 5, 9223372036854775807): check([], command, 'sample-missing', amount)
            c.socket.sendall(c.encode((command, 'sample-missing', 0)))
            assert c.stream.readline() == b'*0\r\n'
            count += 1
            error(b'ERR syntax error', command, 'sample-missing', 0, 'extra')
            error(b'ERR wrong number of arguments', command)
            error(b'WRONGTYPE ', command, 'sample-wrong', 0)
        check([], 'SRANDMEMBER', 'sample-missing', -5)
        for bad in (b'', b'01', b'-0', b'+1', b'1\0', b'9223372036854775808'):
            error(b'ERR value is not an integer or out of range', 'SRANDMEMBER', 'sample-wrong', bad)
            error(b'ERR value is out of range, must be positive', 'SPOP', 'sample-wrong', bad)
        error(b'ERR value is out of range, value must between', 'SRANDMEMBER', 'sample-missing', '-9223372036854775808')
        error(b'ERR value is out of range, must be positive', 'SPOP', 'sample-missing', '-9223372036854775808')
        error(b'ERR value is out of range, must be positive', 'SPOP', 'sample-wrong', -1)
        error(b'ERR wrong number of arguments', 'SINTERCARD')
        check(1, 'SADD', 'sample-single', b'')
        check(1, 'PEXPIREAT', 'sample-single', deadline)
        check(b'', 'SRANDMEMBER', 'sample-single')
        check([b''], 'SRANDMEMBER', 'sample-single', 9223372036854775807)
        check([b''] * 7, 'SRANDMEMBER', 'sample-single', -7)
        check(deadline, 'PEXPIRETIME', 'sample-single')
        check([], 'SPOP', 'sample-single', 0)
        check(deadline, 'PEXPIRETIME', 'sample-single')
        check(b'', 'SPOP', 'sample-single')
        check(b'none', 'TYPE', 'sample-single')
        check(4, 'SADD', 'sample-read', *sorted(values))
        check(1, 'PEXPIREAT', 'sample-read', deadline)
        for amount in (0, 1, 2, 3, 4, 5, 9223372036854775807, -1, -2, -7, -31):
            reply = c.call('SRANDMEMBER', 'sample-read', amount)
            assert isinstance(reply, list) and set(reply) <= values, (amount, reply)
            assert len(reply) == (min(amount, 4) if amount >= 0 else -amount)
            if amount >= 0: assert len(set(reply)) == len(reply)
            count += 3 if amount >= 0 else 2
            members(values, 'sample-read')
            check(deadline, 'PEXPIRETIME', 'sample-read')
        for _ in range(128):
            assert c.call('SRANDMEMBER', 'sample-read') in values
            count += 1
        for amount in (0, 1, 2, 3, 4, 5, 9223372036854775807):
            check(4, 'SADD', 'sample-pop', *sorted(values))
            check(1, 'PEXPIREAT', 'sample-pop', deadline)
            reply = c.call('SPOP', 'sample-pop', amount)
            assert isinstance(reply, list) and len(reply) == len(set(reply)) == min(amount, 4) and set(reply) <= values
            count += 1
            rest = values - set(reply)
            members(rest, 'sample-pop')
            check(deadline if rest else -2, 'PEXPIRETIME', 'sample-pop')
            check(int(bool(rest)), 'DEL', 'sample-pop')
        # Count-only intersections agree with an independent model across duplicates.
        rng = random.Random(12319)
        names = ['card-model-a', 'card-model-b', 'card-model-c']
        models = {name: set() for name in names}
        for step in range(120):
            name, token = rng.choice(names), bytes([rng.randrange(8)])
            if rng.randrange(2):
                new = int(token not in models[name]); models[name].add(token)
                check(new, 'SADD', name, token)
            else:
                old = int(token in models[name]); models[name].discard(token)
                check(old, 'SREM', name, token)
            order = [rng.choice(names) for _ in range(4)]
            expected = set(models[order[0]])
            for other in order[1:]: expected.intersection_update(models[other])
            limit = rng.randrange(5)
            check(min(len(expected), limit) if limit else len(expected), 'SINTERCARD', len(order), *order, 'LIMIT', limit)
        tokens = {('random-pop:%d' % i).encode() + b'\0' for i in range(480)}
        check(480, 'SADD', 'sample-concurrent', *sorted(tokens))
        check(1, 'PEXPIREAT', 'sample-concurrent', deadline)
    finally: c.close()
    start = threading.Barrier(8)
    def worker(number):
        client = Client(port)
        popped, checks = [], 0
        try:
            start.wait(timeout=10)
            while True:
                reply = client.call('SPOP', 'sample-concurrent', 3)
                checks += 1
                assert isinstance(reply, list) and len(reply) == len(set(reply)) <= 3 and set(reply) <= tokens
                if not reply: break
                popped.extend(reply)
            return popped, checks
        finally: client.close()
    with ThreadPoolExecutor(max_workers=8) as pool: outcomes = list(pool.map(worker, range(8)))
    popped = [value for output, _ in outcomes for value in output]
    assert len(popped) == len(set(popped)) == 480 and set(popped) == tokens
    count += sum(checks for _, checks in outcomes) + 1
    c = Client(port)
    try: check(b'none', 'TYPE', 'sample-concurrent')
    finally: c.close()
    return count


def run_sample_size_checks(port):
    # Never send this allocation request to a reference process. pkv rejects a
    # replacement sample that cannot be represented by its response vector.
    client = Client(port)
    try:
        assert client.call('SADD', 'sample-size-safety', b'') == 1
        assert client.call('PEXPIREAT', 'sample-size-safety', 4102444800123) == 1
        reply = client.call('SRANDMEMBER', 'sample-size-safety', -9223372036854775807)
        assert reply == ErrorReply(b'ERR requested sample is too large'), reply
        assert client.call('SMEMBERS', 'sample-size-safety') == [b'']
        assert client.call('PEXPIRETIME', 'sample-size-safety') == 4102444800123
        assert client.call('PING') == b'PONG'
        return 6
    finally: client.close()


def run_scans(port):
    c = Client(port)
    scenarios = 0
    def error(prefix, *args):
        nonlocal scenarios
        actual = c.call(*args)
        assert isinstance(actual, ErrorReply) and actual.startswith(prefix), (args, actual)
        assert c.call('PING') == b'PONG'
        scenarios += 1
    def scan_all(command, key=None, options=(), portable=False, mutate=None):
        nonlocal scenarios
        cursor, seen = b'0', []
        for page in range(10000):
            client = Client(port) if portable else c
            try: reply = client.call(command, *((key,) if key is not None else ()), cursor, *options)
            finally:
                if portable: client.close()
            assert isinstance(reply, list) and len(reply) == 2 and isinstance(reply[0], bytes) and reply[0].isdigit(), reply
            assert isinstance(reply[1], list) and all(isinstance(item, bytes) for item in reply[1]), reply
            cursor = reply[0]
            seen.extend(reply[1])
            if mutate: mutate(page)
            if cursor == b'0':
                scenarios += 1
                return seen
        raise AssertionError(('scan did not terminate', command, key))
    try:
        keys = {('scan-test:%d' % i).encode() + b'\0' for i in range(180)}
        for key in keys: assert c.call('SET', key, b'value') == b'OK'
        assert c.call('SET', 'scan-test:expired', 'value', 'PXAT', 1) == b'OK'
        assert set(scan_all('SCAN', options=('COUNT', 7, 'MATCH', b'scan-test:*'), portable=True)) == keys
        assert scan_all('SCAN', options=('MATCH', 'scan-test:no-match-*', 'COUNT', 3)) == []
        names = {b'', b'a', b'ab', b'abc', b'aa', b'ac', b'a\0', b'*', b'?', b'[', b']', b'\\', b'\0', b'\x80', b'\xff'}
        pairs = [part for name in sorted(names) for part in (name, b'value:' + name)]
        assert c.call('HSET', 'scan-hash', *pairs) == len(names)
        assert c.call('SADD', 'scan-set', *sorted(names)) == len(names)
        single = {name for name in names if len(name) == 1}
        patterns = [(b'*', names), (b'', {b''}), (b'?', single), (b'a*', {name for name in names if name.startswith(b'a')}),
                    (b'a?', {name for name in names if len(name) == 2 and name.startswith(b'a')}),
                    (b'[a-c]', {b'a'}), (b'[^a]', single - {b'a'}), (b'\\*', {b'*'}), (b'\\?', {b'?'}),
                    (b'\\[', {b'['}), (b'\\', {b'\\'}), (b'[c-a]', {b'a'}), (b'[]', set()),
                    (b'[a', {b'a'}), (b'[^', single), (b'[a-]', {b'a', b']'}),
                    (b'[\x80-\xff]', {b'\x80', b'\xff'}), (b'[\0-\xff]', {b'\0', b'\xff'}),
                    (b'a\0', {b'a\0'}), (b'*?*?*', {name for name in names if len(name) >= 2})]
        for pattern, expected in patterns:
            values = scan_all('HSCAN', 'scan-hash', ('MATCH', pattern, 'COUNT', 1))
            assert len(values) % 2 == 0 and dict(zip(values[::2], values[1::2])) == {name: b'value:' + name for name in expected}, (pattern, values)
            assert set(scan_all('HSCAN', 'scan-hash', ('NOVALUES', 'MATCH', pattern, 'COUNT', 1))) == expected, pattern
            assert set(scan_all('SSCAN', 'scan-set', ('MATCH', pattern, 'COUNT', 1))) == expected, pattern
        # Large collections exercise continuation, independent cursors and portability.
        stable = {('field:%d' % i).encode() + b'\0' for i in range(220)}
        fields = [part for name in sorted(stable) for part in (name, b'payload:' + name)]
        assert c.call('HSET', 'scan-large-hash', *fields) == 220
        assert c.call('SADD', 'scan-large-set', *sorted(stable)) == 220
        def churn_hash(page):
            c.call('HDEL', 'scan-large-hash', 'transient:%d' % (page - 1))
            c.call('HSET', 'scan-large-hash', 'transient:%d' % page, 'value')
        def churn_set(page):
            c.call('SREM', 'scan-large-set', 'transient:%d' % (page - 1))
            c.call('SADD', 'scan-large-set', 'transient:%d' % page)
        assert set(scan_all('HSCAN', 'scan-large-hash', ('COUNT', 1, 'NOVALUES'), True, churn_hash)) >= stable
        assert set(scan_all('SSCAN', 'scan-large-set', ('COUNT', 1), True, churn_set)) >= stable
        assert c.call('PEXPIREAT', 'scan-large-hash', 4102444800123) == 1
        assert c.call('PEXPIREAT', 'scan-large-set', 4102444800456) == 1
        scan_all('HSCAN', 'scan-large-hash', ('COUNT', 17))
        scan_all('SSCAN', 'scan-large-set', ('COUNT', 17))
        assert c.call('PEXPIRETIME', 'scan-large-hash') == 4102444800123
        assert c.call('PEXPIRETIME', 'scan-large-set') == 4102444800456
        # TYPE filters and repeated options use the last value.
        for command, key, args in [('SET', 'scan-type:string', ('value',)), ('HSET', 'scan-type:hash', ('f', 'v')),
                                   ('SADD', 'scan-type:set', ('v',)), ('RPUSH', 'scan-type:list', ('v',))]:
            assert c.call(command, key, *args) in (b'OK', 1)
        for typename in ('string', 'hash', 'set', 'list'):
            assert set(scan_all('SCAN', options=('TYPE', typename.upper(), 'MATCH', 'scan-type:*', 'COUNT', 3))) == {('scan-type:' + typename).encode()}
        assert scan_all('SCAN', options=('TYPE', 'not-a-real-type', 'MATCH', 'scan-type:*')) == []
        assert set(scan_all('SSCAN', 'scan-set', ('MATCH', 'no-match', 'MATCH', '*', 'COUNT', 1, 'COUNT', 100))) == names
        for command, key in [('HSCAN', 'scan-hash'), ('SSCAN', 'scan-set'), ('SCAN', None)]:
            prefix = (key,) if key is not None else ()
            for cursor in ('00', '+0', '-0', ' 0'):
                reply = c.call(command, *prefix, cursor, 'COUNT', 9223372036854775807)
                assert isinstance(reply, list) and reply[0] == b'0'
                scenarios += 1
            for cursor in (b'', b'-1', b'x', b'1x', b'18446744073709551616'):
                error(b'ERR invalid cursor', command, *prefix, cursor)
            for args in [('COUNT', 0), ('COUNT', -1), ('COUNT',), ('MATCH',), ('UNKNOWN', 1), ('COUNT', 1, 'UNKNOWN')]:
                error(b'ERR syntax error', command, *prefix, 0, *args)
            for count in ('bad', '01', '-0', '9223372036854775808'):
                error(b'ERR value is not an integer or out of range', command, *prefix, 0, 'COUNT', count)
            error(b'ERR wrong number of arguments', command)
        error(b'ERR NOVALUES option can only be used in HSCAN', 'SCAN', 0, 'NOVALUES')
        error(b'ERR NOVALUES option can only be used in HSCAN', 'SSCAN', 'scan-set', 0, 'NOVALUES')
        assert c.call('SET', 'scan-wrong', 'string') == b'OK'
        for command in ('HSCAN', 'SSCAN'):
            error(b'ERR invalid cursor', command, 'scan-wrong', 'bad')
            error(b'WRONGTYPE ', command, 'scan-wrong', 0, 'COUNT', 'bad')
            assert c.call(command, 'scan-missing', 0, 'COUNT', 'bad') == [b'0', []]
            scenarios += 1
        assert c.call('PEXPIREAT', 'scan-hash', 1) == 1
        assert c.call('HSCAN', 'scan-hash', 0) == [b'0', []]
        scenarios += 1
    finally: c.close()
    return scenarios


def run_scan_safety_checks(port):
    # The local reference parser uses strlen for cursor input. Preserve full
    # byte validation rather than silently truncating an embedded NUL suffix.
    client = Client(port)
    try:
        for args in [('SCAN', b'0\0suffix'), ('HSCAN', 'missing', b'0\0suffix'), ('SSCAN', 'missing', b'0\0suffix'), ('ZSCAN', 'missing', b'0\0suffix')]:
            reply = client.call(*args)
            assert reply == ErrorReply(b'ERR invalid cursor'), (args, reply)
            assert client.call('PING') == b'PONG'
        return 8
    finally: client.close()


def run_database_configuration(binary, redis):
    with server(os.path.abspath(binary), redis, False, databases=3) as port:
        client = Client(port)
        try:
            assert client.call('SELECT', 2) == b'OK'
            assert client.call('SET', 'configuration-token', 'value') == b'OK'
            assert client.call('SELECT', 3) == ErrorReply(b'ERR DB index is out of range')
            assert client.call('GET', 'configuration-token') == b'value'
            assert client.call('COPY', 'configuration-token', 'configuration-copy', 'DB', 3) == ErrorReply(b'ERR DB index is out of range')
            assert client.call('GET', 'configuration-token') == b'value'
            assert client.call('COPY', 'configuration-token', 'configuration-copy', 'DB', 0) == 1
            assert client.call('SELECT', 0) == b'OK'
            assert client.call('GET', 'configuration-copy') == b'value'
        finally: client.close()
    return 9


def run_logical_databases(port):
    c, other = Client(port), Client(port)
    checks = 0
    def check(expected, *args, client=None):
        nonlocal checks
        actual = (client or c).call(*args)
        assert type(actual) is type(expected) and actual == expected, (args, actual, expected)
        checks += 1
    def error(prefix, *args):
        nonlocal checks
        actual = c.call(*args)
        assert isinstance(actual, ErrorReply) and actual.startswith(prefix), (args, actual)
        checks += 1
    deadline = 4102444800123
    binary = bytes(range(256))
    try:
        check(b'OK', 'SET', 'dbspace:same', 'zero')
        check(b'OK', 'SELECT', 1)
        check(None, 'GET', 'dbspace:same')
        check(b'OK', 'SET', 'dbspace:same', 'one')
        check(b'zero', 'GET', 'dbspace:same', client=other)
        check(b'OK', 'SELECT', 1, client=other)
        check(b'one', 'GET', 'dbspace:same', client=other)
        for index in (2, 15, 0, 1): check(b'OK', 'SELECT', index)
        for index in (-1, 16, 2147483647): error(b'ERR DB index is out of range', 'SELECT', index)
        for index in ('bad', '01', '-0', '+1', b'1\0bad', 9223372036854775808): error(b'ERR value is not an integer or out of range', 'SELECT', index)
        for index in (-2147483649, 2147483648): error(b'ERR value is out of range', 'SELECT', index)
        check(b'one', 'GET', 'dbspace:same')
        c.socket.sendall(Client.encode(('SELECT', 2)) + Client.encode(('SET', 'dbspace:pipeline', 'two')) + Client.encode(('SELECT', 0)) + Client.encode(('GET', 'dbspace:pipeline')))
        assert [c.read() for _ in range(4)] == [b'OK', b'OK', b'OK', None]
        checks += 4
        fixtures = [('string', ('SET', binary), ('GET',), binary),
                    ('hash', ('HSET', b'', binary), ('HGET', b''), binary),
                    ('list', ('RPUSH', b'', binary), ('LRANGE', 0, -1), [b'', binary]),
                    ('set', ('SADD', b'', binary), ('SMEMBERS',), sorted([b'', binary])),
                    ('zset', ('ZADD', 1, b'', 2, binary), ('ZRANGE', 0, -1, 'WITHSCORES'), [b'', b'1', binary, b'2'])]
        for index, (kind, create, read, expected) in enumerate(fixtures):
            source = b'dbspace:copy:%d\0' % index + binary
            target = b'dbspace:target:%d\0' % index
            check(b'OK', 'SELECT', 0)
            c.call(create[0], source, *create[1:])
            check(1, 'PEXPIREAT', source, deadline)
            check(1, 'COPY', source, source, 'DB', 2)
            check(1, 'COPY', source, target)
            check(0, 'COPY', source, target)
            check(1, 'COPY', source, target, 'REPLACE', 'REPLACE')
            check(deadline, 'PEXPIRETIME', source)
            check(b'OK', 'SELECT', 2)
            check(kind.encode(), 'TYPE', source)
            actual = c.call(read[0], source, *read[1:])
            if kind == 'set': actual = sorted(actual)
            assert actual == expected, (kind, actual, expected)
            checks += 1
            check(deadline, 'PEXPIRETIME', source)
            check(b'OK', 'SELECT', 0)
            check(1, 'DEL', source)
            check(b'OK', 'SELECT', 2)
            check(kind.encode(), 'TYPE', source)
        check(b'OK', 'SELECT', 0)
        check(b'OK', 'SET', 'dbspace:persistent', 'new')
        check(b'OK', 'SELECT', 3)
        check(b'OK', 'SET', 'dbspace:replace', 'old', 'PXAT', deadline)
        check(b'OK', 'SELECT', 0)
        check(0, 'COPY', 'dbspace:persistent', 'dbspace:replace', 'DB', 3)
        check(1, 'COPY', 'dbspace:persistent', 'dbspace:replace', 'DB', 3, 'REPLACE')
        check(b'OK', 'SELECT', 3)
        check(b'new', 'GET', 'dbspace:replace')
        check(-1, 'PTTL', 'dbspace:replace')
        check(b'OK', 'SELECT', 0)
        check(1, 'COPY', 'dbspace:persistent', 'dbspace:repeated', 'DB', 1, 'DB', 3)
        check(None, 'GET', 'dbspace:repeated')
        check(0, 'COPY', 'dbspace:absent', 'dbspace:replace', 'DB', 3, 'REPLACE')
        check(b'OK', 'SET', 'dbspace:expired', 'old', 'PXAT', 1)
        check(0, 'COPY', 'dbspace:expired', 'dbspace:replace', 'DB', 3, 'REPLACE')
        check(b'OK', 'SELECT', 3)
        check(b'new', 'GET', 'dbspace:repeated')
        check(b'new', 'GET', 'dbspace:replace')
        check(b'OK', 'SET', 'dbspace:expired-target', 'old', 'PXAT', 1)
        check(b'OK', 'SELECT', 0)
        check(1, 'COPY', 'dbspace:persistent', 'dbspace:expired-target', 'DB', 3)
        for name in ('dbspace:absent', 'dbspace:persistent'):
            error(b'ERR source and destination objects are the same', 'COPY', name, name)
            error(b'ERR source and destination objects are the same', 'COPY', name, name, 'REPLACE')
        for args in [('DB',), ('UNKNOWN',), ('REPLACE', 'extra')]: error(b'ERR syntax error', 'COPY', 'dbspace:absent', 'other', *args)
        for index in (-1, 16): error(b'ERR DB index is out of range', 'COPY', 'dbspace:absent', 'other', 'DB', index)
        error(b'ERR DB index is out of range', 'COPY', 'dbspace:absent', 'other', 'DB', 16, 'DB', 0)
        for index in ('bad', '+1', b'1\0bad'): error(b'ERR value is not an integer or out of range', 'COPY', 'dbspace:absent', 'other', 'DB', index)
        error(b'ERR value is out of range', 'COPY', 'dbspace:absent', 'other', 'DB', 2147483648)
        for command, args in [('COPY', ()), ('COPY', ('a',)), ('SELECT', ()), ('SELECT', (0, 1))]: error(b'ERR wrong number of arguments', command, *args)
        # Every command family and scan consults the connection's selected DB.
        for index in (4, 5):
            check(b'OK', 'SELECT', index)
            check(1, 'HSET', 'dbspace:hash', 'field', str(index))
            check(1, 'RPUSH', 'dbspace:list', str(index))
            check(1, 'SADD', 'dbspace:set', str(index))
            check(1, 'ZADD', 'dbspace:zset', index, 'member')
        for index in (4, 5):
            check(b'OK', 'SELECT', index)
            check(str(index).encode(), 'HGET', 'dbspace:hash', 'field')
            check([str(index).encode()], 'LRANGE', 'dbspace:list', 0, -1)
            check([str(index).encode()], 'SMEMBERS', 'dbspace:set')
            check(str(index).encode(), 'ZSCORE', 'dbspace:zset', 'member')
            cursor, found = b'0', []
            while True:
                cursor, items = c.call('SCAN', cursor, 'MATCH', 'dbspace:*', 'COUNT', 1)
                found.extend(items)
                if cursor == b'0': break
            assert set(found) == {b'dbspace:hash', b'dbspace:list', b'dbspace:set', b'dbspace:zset'}
            checks += 1
        # Opposite-direction copies lock DB identities in one consistent order.
        start = threading.Barrier(6)
        def worker(index):
            client = Client(port)
            try:
                assert client.call('SELECT', 6 + index % 2) == b'OK'
                start.wait(timeout=10)
                for iteration in range(65):
                    assert client.call('SET', 'dbspace:race-source', '%d:%d' % (index, iteration)) == b'OK'
                    assert client.call('COPY', 'dbspace:race-source', 'dbspace:race-source', 'DB', 7 - index % 2, 'REPLACE') == 1
                    value = client.call('GET', 'dbspace:race-source')
                    parts = value.split(b':')
                    assert len(parts) == 2 and all(part.isdigit() for part in parts) and 0 <= int(parts[0]) < 6 and 0 <= int(parts[1]) < 65, value
                return 196
            finally: client.close()
        with ThreadPoolExecutor(max_workers=6) as pool: checks += sum(pool.map(worker, range(6)))
        check(b'OK', 'SELECT', 0)
    finally: c.close(); other.close()
    return checks


def run_renames(port):
    c = Client(port)
    checks = 0
    def check(expected, *args):
        nonlocal checks
        actual = c.call(*args)
        assert type(actual) is type(expected) and actual == expected, (args, actual, expected)
        checks += 1
    def error(prefix, *args):
        nonlocal checks
        actual = c.call(*args)
        assert isinstance(actual, ErrorReply) and actual.startswith(prefix), (args, actual)
        checks += 1
    binary = bytes(range(256))
    deadline = 4102444800123
    fixtures = [('string', ('SET', binary), ('GET',), binary),
                ('hash', ('HSET', b'', binary, b'f\0', b''), ('HGETALL',), {b'': binary, b'f\0': b''}),
                ('list', ('RPUSH', b'', binary, b'last\0', b''), ('LRANGE', 0, -1), [b'', binary, b'last\0', b'']),
                ('set', ('SADD', b'', binary, b'member\0'), ('SMEMBERS',), sorted([b'', binary, b'member\0'])),
                ('zset', ('ZADD', 1, b'', 2, binary, -1, b'first\0'), ('ZRANGE', 0, -1, 'WITHSCORES'), [b'first\0', b'-1', b'', b'1', binary, b'2'])]
    def contents(kind, command, key, expected):
        nonlocal checks
        actual = c.call(command[0], key, *command[1:])
        if kind == 'hash': actual = dict(zip(actual[::2], actual[1::2]))
        if kind == 'set': actual = sorted(actual)
        assert actual == expected, (kind, key, actual, expected)
        checks += 1
    try:
        for command in ('RENAME', 'RENAMENX'):
            error(b'ERR no such key', command, 'rename-missing', 'rename-target')
            error(b'ERR no such key', command, 'rename-missing', 'rename-missing')
        for i, (kind, create, read, expected) in enumerate(fixtures):
            for expiring in (False, True):
                source = b'rename-source:%d:%d\0' % (i, expiring) + binary
                destination = b'rename-destination:%d:%d\0' % (i, expiring) + binary
                c.call(create[0], source, *create[1:])
                if expiring: check(1, 'PEXPIREAT', source, deadline)
                check(b'OK', 'RENAME', source, source)
                check(0, 'RENAMENX', source, source)
                contents(kind, read, source, expected)
                check(deadline if expiring else -1, 'PEXPIRETIME', source)
                check(b'OK', 'SET', destination, 'old')
                check(1, 'PEXPIREAT', destination, deadline + 999)
                check(0, 'RENAMENX', source, destination)
                contents(kind, read, source, expected)
                check(b'old', 'GET', destination)
                check(deadline + 999, 'PEXPIRETIME', destination)
                check(b'OK', 'RENAME', source, destination)
                check(b'none', 'TYPE', source)
                check(kind.encode(), 'TYPE', destination)
                contents(kind, read, destination, expected)
                check(deadline if expiring else -1, 'PEXPIRETIME', destination)
                check(b'OK', 'RENAME', destination, source)
                check(1, 'RENAMENX', source, destination)
                contents(kind, read, destination, expected)
                check(b'none', 'TYPE', source)
        check(b'OK', 'SET', 'rename-expired-source', 'old', 'PXAT', 1)
        check(b'OK', 'SET', 'rename-expired-destination', 'keep')
        for command in ('RENAME', 'RENAMENX'):
            error(b'ERR no such key', command, 'rename-expired-source', 'rename-expired-destination')
            check(b'keep', 'GET', 'rename-expired-destination')
        check(b'OK', 'SET', 'rename-live', 'move', 'PXAT', deadline)
        check(b'OK', 'SET', 'rename-expired-destination', 'old', 'PXAT', 1)
        check(1, 'RENAMENX', 'rename-live', 'rename-expired-destination')
        check(b'move', 'GET', 'rename-expired-destination')
        check(deadline, 'PEXPIRETIME', 'rename-expired-destination')
        check(b'OK', 'SET', b'', binary)
        check(b'OK', 'RENAME', b'', b'rename-empty-key\0')
        check(binary, 'GET', b'rename-empty-key\0')
        check(1, 'RENAMENX', b'rename-empty-key\0', b'')
        check(binary, 'GET', b'')
        check(1, 'DEL', b'')
        for command in ('RENAME', 'RENAMENX'):
            for args in [(), ('rename-live',), ('rename-live', 'rename-target', 'extra')]:
                error(b'ERR wrong number of arguments', command, *args)
        rng = random.Random(189337)
        keys = [b'rename-model:%d' % i for i in range(7)]
        model = {}
        for step in range(200):
            source, destination = rng.choice(keys), rng.choice(keys)
            if step % 3 == 0:
                value, expiry = b'value:%d\0' % step, deadline if rng.randrange(2) else -1
                check(b'OK', 'SET', source, value, *(("PXAT", expiry) if expiry >= 0 else ()))
                model[source] = (value, expiry)
            else:
                nx = bool(rng.randrange(2))
                command = 'RENAMENX' if nx else 'RENAME'
                if source not in model: error(b'ERR no such key', command, source, destination)
                elif nx and destination in model: check(0, command, source, destination)
                else:
                    check(1 if nx else b'OK', command, source, destination)
                    if source != destination: model[destination] = model.pop(source)
            check([model[k][0] if k in model else None for k in keys], 'MGET', *keys)
            for key in keys: check(model[key][1] if key in model else -2, 'PEXPIRETIME', key)
        for round_number in range(12):
            source = 'rename-nx-race:%d' % round_number
            destinations = ['rename-nx-result:%d:%d' % (round_number, i) for i in range(8)]
            check(b'OK', 'SET', source, 'token', 'PXAT', deadline)
            start = threading.Barrier(8)
            def race(index):
                client = Client(port)
                try:
                    start.wait(timeout=10)
                    return client.call('RENAMENX', source, destinations[index])
                finally: client.close()
            with ThreadPoolExecutor(max_workers=8) as pool: replies = list(pool.map(race, range(8)))
            assert sum(type(reply) is int and reply == 1 for reply in replies) == 1, replies
            assert all(reply == 1 or reply == ErrorReply(b'ERR no such key') for reply in replies), replies
            checks += 2
            check(1, 'EXISTS', source, *destinations)
            actual = c.call('MGET', *destinations)
            assert actual.count(b'token') == 1 and actual.count(None) == 7, actual
            checks += 1
            check(deadline, 'PEXPIRETIME', destinations[replies.index(1)])
        # MGET must see the token at exactly one key during every rename commit.
        check(b'OK', 'SET', 'rename-baton:a', 'token')
        start = threading.Barrier(5)
        def baton(index):
            client = Client(port)
            try:
                start.wait(timeout=10)
                for iteration in range(120):
                    if index == 0:
                        source, destination = ('rename-baton:a', 'rename-baton:b') if iteration % 2 == 0 else ('rename-baton:b', 'rename-baton:a')
                        assert client.call('RENAME', source, destination) == b'OK'
                    else:
                        actual = client.call('MGET', 'rename-baton:a', 'rename-baton:b')
                        assert actual in ([b'token', None], [None, b'token']), actual
                return 120
            finally: client.close()
        with ThreadPoolExecutor(max_workers=5) as pool: checks += sum(pool.map(baton, range(5)))
        check([b'token', None], 'MGET', 'rename-baton:a', 'rename-baton:b')
    finally: c.close()
    return checks


def run_zset_algebra(port):
    c = Client(port)
    checks = 0
    def check(expected, *args):
        nonlocal checks
        actual = c.call(*args)
        assert type(actual) is type(expected) and actual == expected, (args, actual, expected)
        checks += 1
    def error(prefix, *args):
        nonlocal checks
        actual = c.call(*args)
        assert isinstance(actual, ErrorReply) and actual.startswith(prefix), (args, actual)
        checks += 1
    def result(expected, *args):
        nonlocal checks
        actual = c.call(*args)
        ordered = sorted(expected.items(), key=lambda item: (item[1], item[0]))
        scores = any(isinstance(arg, str) and arg.upper() == 'WITHSCORES' for arg in args)
        assert isinstance(actual, list), (args, actual)
        assert actual[::2] == [m for m, s in ordered] if scores else actual == [m for m, s in ordered], (args, actual, ordered)
        if scores:
            assert len(actual) == len(ordered) * 2, (args, actual)
            for text, (member, score) in zip(actual[1::2], ordered):
                # The reference's compact encoding may canonicalize signed zero.
                expected_text = (str(int(score)) if score == int(score) else str(score)).encode() if abs(score) != float('inf') else (b'-inf' if score < 0 else b'inf')
                assert text == expected_text or score == 0 and text in (b'0', b'-0'), (args, actual, ordered)
        checks += 1
    def model(operation, sources, weights, aggregate):
        order = list(zip(sources, weights))
        if operation != 'ZDIFF': order.sort(key=lambda item: len(item[0]))
        if operation == 'ZUNION': members = set().union(*(set(src) for src in sources))
        elif operation == 'ZINTER': members = set(sources[0]).intersection(*(set(src) for src in sources[1:]))
        else: return {m: s for m, s in sources[0].items() if all(m not in src for src in sources[1:])}
        out = {}
        for member in members:
            values = [src[member] * weight for src, weight in order if member in src]
            out[member] = sum(values) if aggregate == 'SUM' else min(values) if aggregate == 'MIN' else max(values)
        return out
    names = ['zalgebra-a', 'zalgebra-b', 'zalgebra-set']
    values = [{b'': 1, b'a\0': 2, bytes(range(256)): -3}, {b'': 3, b'a\0': -1, b'\xff': 5}, {b'': 1, b'a\0': 1, b'only-set': 1}]
    deadline = 4102444800123
    try:
        for key, entries in zip(names[:2], values[:2]): check(len(entries), 'ZADD', key, *[part for m, s in entries.items() for part in (s, m)])
        check(3, 'SADD', names[2], *values[2])
        for key in names: check(1, 'PEXPIREAT', key, deadline)
        for operation in ('ZUNION', 'ZINTER', 'ZDIFF'):
            order = [0, 2, 1, 0]
            for aggregate in ('SUM', 'MIN', 'MAX'):
                weights = [2, -1, 0, 1]
                flags = () if operation == 'ZDIFF' else ('WEIGHTS', *weights, 'AGGREGATE', aggregate)
                expected = model(operation, [values[i] for i in order], weights, aggregate)
                result(expected, operation, 4, *[names[i] for i in order], *flags, 'WITHSCORES')
                result(expected, operation, 4, *[names[i] for i in order], *flags)
                check(b'OK', 'SET', 'zalgebra-destination', 'old')
                check(1, 'PEXPIREAT', 'zalgebra-destination', deadline)
                check(len(expected), operation + 'STORE', 'zalgebra-destination', 4, *[names[i] for i in order], *flags)
                result(expected, 'ZRANGE', 'zalgebra-destination', 0, -1, 'WITHSCORES')
                check(-1 if expected else -2, 'PTTL', 'zalgebra-destination')
        for key in names: check(deadline, 'PEXPIRETIME', key)
        check(b'set', 'TYPE', names[2])
        result({b'': 2, b'a\0': 3, bytes(range(256)): -3, b'only-set': 1}, 'ZUNION', 2, names[0], names[2], 'WEIGHTS', 9, 9, 'WEIGHTS', 1, 1, 'AGGREGATE', 'MIN', 'AGGREGATE', 'SUM', 'WITHSCORES', 'WITHSCORES')
        check(2, 'ZINTERCARD', 4, names[0], names[1], names[2], names[0])
        for limit in (0, 1, 2, 3, 9223372036854775807): check(min(limit, 2) if limit else 2, 'ZINTERCARD', 3, *names, 'LIMIT', limit)
        check(2, 'ZINTERCARD', 3, *names, 'LIMIT', 1, 'LIMIT', 0)
        # Aliasing a source/destination must read the old collection completely.
        check(3, 'ZUNIONSTORE', 'zalgebra-alias', 1, names[2])
        check(b'zset', 'TYPE', 'zalgebra-alias')
        check(1, 'PEXPIREAT', 'zalgebra-alias', deadline)
        check(3, 'ZUNIONSTORE', 'zalgebra-alias', 2, 'zalgebra-alias', 'zalgebra-alias', 'WEIGHTS', 2, 3)
        result({m: s * 5 for m, s in values[2].items()}, 'ZRANGE', 'zalgebra-alias', 0, -1, 'WITHSCORES')
        check(-1, 'PTTL', 'zalgebra-alias')
        check(0, 'ZDIFFSTORE', 'zalgebra-alias', 2, 'zalgebra-alias', 'zalgebra-alias')
        check(b'none', 'TYPE', 'zalgebra-alias')
        check(1, 'ZADD', 'zalgebra-expired', 1, 'old')
        check(1, 'PEXPIREAT', 'zalgebra-expired', 1)
        for operation in ('ZUNION', 'ZINTER', 'ZDIFF'):
            result({} if operation != 'ZUNION' else values[0], operation, 2, 'zalgebra-expired', names[0], 'WITHSCORES')
        check(b'OK', 'SET', 'zalgebra-wrong', 'string')
        for operation in ('ZUNION', 'ZINTER', 'ZDIFF', 'ZINTERCARD'):
            error(b'WRONGTYPE ', operation, 2, 'zalgebra-missing', 'zalgebra-wrong', 'UNKNOWN')
        check(b'OK', 'SET', 'zalgebra-destination', 'keep')
        for operation in ('ZUNIONSTORE', 'ZINTERSTORE', 'ZDIFFSTORE'):
            error(b'WRONGTYPE ', operation, 'zalgebra-destination', 2, 'zalgebra-missing', 'zalgebra-wrong', 'UNKNOWN')
            check(b'keep', 'GET', 'zalgebra-destination')
        for operation in ('ZUNION', 'ZINTER', 'ZDIFF', 'ZINTERCARD', 'ZUNIONSTORE', 'ZINTERSTORE', 'ZDIFFSTORE'):
            prefix = (operation, 'zalgebra-destination') if operation.endswith('STORE') else (operation,)
            error(b'ERR at least 1 input key is needed', *prefix, 0, names[0])
            for numkeys in ('bad', '01', '+1', '9223372036854775808'):
                error(b'ERR value is not an integer or out of range', *prefix, numkeys, names[0])
            error(b'ERR syntax error', *prefix, 9223372036854775807, names[0])
            error(b'ERR wrong number of arguments', operation)
        for operation in ('ZUNION', 'ZINTER'):
            for flags in [('WEIGHTS',), ('AGGREGATE',), ('AGGREGATE', 'bad'), ('UNKNOWN',), ('LIMIT', 1)]:
                error(b'ERR syntax error', operation, 1, names[0], *flags)
            for weight in ('bad', 'nan', '1e999', '1e-999', b'1\0bad'):
                error(b'ERR weight value is not a float', operation, 1, names[0], 'WEIGHTS', weight)
        for flags in [('WEIGHTS', 1), ('AGGREGATE', 'SUM')]: error(b'ERR syntax error', 'ZDIFF', 1, names[0], *flags)
        for flags in [('WEIGHTS', 1), ('WITHSCORES',), ('LIMIT',), ('AGGREGATE', 'SUM')]: error(b'ERR syntax error', 'ZINTERCARD', 1, names[0], *flags)
        for limit in (-1, 'bad', '01', 9223372036854775808): error(b"ERR LIMIT can't be negative", 'ZINTERCARD', 1, names[0], 'LIMIT', limit)
        for operation in ('ZUNIONSTORE', 'ZINTERSTORE', 'ZDIFFSTORE'): error(b'ERR syntax error', operation, 'zalgebra-destination', 1, names[0], 'WITHSCORES')
        # Derived NaN is normalized by SUM. Intersection MIN/MAX ignore a NaN
        # contribution after the seed; union normalizes every product first.
        check(1, 'ZADD', 'zalgebra-float-small', 5, 'x')
        check(2, 'ZADD', 'zalgebra-float-large', 'inf', 'x', 1, 'other')
        for operation, aggregate, expected in [('ZUNION', 'SUM', 5), ('ZUNION', 'MIN', 0), ('ZUNION', 'MAX', 5),
                                                ('ZINTER', 'SUM', 0), ('ZINTER', 'MIN', 5), ('ZINTER', 'MAX', 5)]:
            expected_values = {b'x': expected}
            if operation == 'ZUNION': expected_values[b'other'] = 0
            result(expected_values, operation, 2, 'zalgebra-float-small', 'zalgebra-float-large', 'WEIGHTS', 1, 0, 'AGGREGATE', aggregate, 'WITHSCORES')
        result({m: s * 1.5 for m, s in values[0].items()}, 'ZUNION', 1, names[0], 'WEIGHTS', '1.5', 'WITHSCORES')
        result({m: s * 2 for m, s in values[0].items()}, 'ZINTER', 1, names[0], 'WEIGHTS', '0x1p1', 'WITHSCORES')
        result({m: s * float('inf') for m, s in values[0].items()}, 'ZUNION', 1, names[0], 'WEIGHTS', 'inf', 'WITHSCORES')
        check(3, 'SADD', 'zalgebra-integer-set', '1', '2', '3')
        check(3, 'ZADD', 'zalgebra-numeric-zset', 5, '1', 6, '01', 7, '2')
        result({b'1': 6, b'2': 8}, 'ZINTER', 2, 'zalgebra-integer-set', 'zalgebra-numeric-zset', 'WITHSCORES')
        check(1, 'SADD', 'zalgebra-integer-set', '01')
        result({b'1': 6, b'01': 7, b'2': 8}, 'ZINTER', 2, 'zalgebra-integer-set', 'zalgebra-numeric-zset', 'WITHSCORES')
        check(3, 'ZINTERCARD', 2, 'zalgebra-integer-set', 'zalgebra-numeric-zset')
        for create, args in [('HSET', ('f', 'v')), ('LPUSH', ('v',))]:
            c.call('DEL', 'zalgebra-other-wrong')
            check(1, create, 'zalgebra-other-wrong', *args)
            error(b'WRONGTYPE ', 'ZUNION', 2, 'zalgebra-missing', 'zalgebra-other-wrong', 'WEIGHTS', 'bad', 'bad')
            error(b'WRONGTYPE ', 'ZINTERCARD', 2, 'zalgebra-missing', 'zalgebra-other-wrong', 'LIMIT', 0)
        check(1, 'ZADD', 'zalgebra-plus-inf', 'inf', 'x')
        check(1, 'ZADD', 'zalgebra-minus-inf', '-inf', 'x')
        for aggregate, expected in [('SUM', 0), ('MIN', -float('inf')), ('MAX', float('inf'))]:
            result({b'x': expected}, 'ZUNION', 2, 'zalgebra-plus-inf', 'zalgebra-minus-inf', 'AGGREGATE', aggregate, 'WITHSCORES')
        for index, score in enumerate((10000000000000000, -10000000000000000, 1)):
            check(index + 1, 'ZADD', 'zalgebra-order:%d' % index, score, 'x', *[part for i in range(index) for part in (1, 'filler:%d:%d' % (index, i))])
        result({b'x': 1}, 'ZINTER', 3, 'zalgebra-order:2', 'zalgebra-order:1', 'zalgebra-order:0', 'WITHSCORES')
        rng = random.Random(917211)
        for step in range(160):
            source = rng.randrange(2)
            member, score = bytes([rng.randrange(24)]) + b'\0', rng.randrange(-9, 10)
            check(int(member not in values[source]), 'ZADD', names[source], score, member)
            values[source][member] = score
            order = [rng.randrange(3) for _ in range(rng.randrange(1, 6))]
            weights = [rng.randrange(-2, 3) for _ in order]
            aggregate = rng.choice(('SUM', 'MIN', 'MAX'))
            for operation in ('ZUNION', 'ZINTER', 'ZDIFF'):
                flags = () if operation == 'ZDIFF' else ('WEIGHTS', *weights, 'AGGREGATE', aggregate)
                expected = model(operation, [values[i] for i in order], weights, aggregate)
                result(expected, operation, len(order), *[names[i] for i in order], *flags, 'WITHSCORES')
                if step % 4 == 0:
                    check(len(expected), operation + 'STORE', 'zalgebra-model-store', len(order), *[names[i] for i in order], *flags)
                    result(expected, 'ZRANGE', 'zalgebra-model-store', 0, -1, 'WITHSCORES')
            cardinality = len(set(values[order[0]]).intersection(*(set(values[i]) for i in order[1:])))
            limit = rng.randrange(4)
            check(min(limit, cardinality) if limit else cardinality, 'ZINTERCARD', len(order), *[names[i] for i in order], 'LIMIT', limit)
        check(1, 'ZADD', 'zalgebra-race-source', 1, 'counter')
        start = threading.Barrier(5)
        def worker(index):
            client = Client(port)
            try:
                start.wait(timeout=10)
                for iteration in range(80):
                    if index == 0: assert client.call('ZINCRBY', 'zalgebra-race-source', 1, 'counter') is not None
                    elif index % 2:
                        reply = client.call('ZUNION', 2, 'zalgebra-race-source', 'zalgebra-race-source', 'WEIGHTS', 1, -1, 'WITHSCORES')
                        assert reply == [b'counter', b'0'], reply
                    else:
                        assert client.call('ZINTERSTORE', 'zalgebra-race-store', 2, 'zalgebra-race-source', 'zalgebra-race-source', 'WEIGHTS', 1, -1) == 1
                        assert client.call('ZSCORE', 'zalgebra-race-store', 'counter') == b'0'
                return 80 if index == 0 or index % 2 else 160
            finally: client.close()
        with ThreadPoolExecutor(max_workers=5) as pool: checks += sum(pool.map(worker, range(5)))
    finally: c.close()
    return checks


def run_zmpops(port):
    c = Client(port)
    checks = 0
    def check(expected, *args):
        nonlocal checks
        actual = c.call(*args)
        assert type(actual) is type(expected) and actual == expected, (args, actual, expected)
        checks += 1
    def error(prefix, *args):
        nonlocal checks
        actual = c.call(*args)
        assert isinstance(actual, ErrorReply) and actual.startswith(prefix), (args, actual)
        checks += 1
    binary = bytes(range(256))
    key = b'zmpop\0' + binary
    deadline = 4102444800123
    try:
        check(None, 'ZMPOP', 2, 'zmpop-absent', key, 'MIN')
        c.socket.sendall(Client.encode(('ZMPOP', 1, 'zmpop-absent', 'MIN')))
        assert c.stream.readline() == b'*-1\r\n'
        checks += 1
        check(4, 'ZADD', key, 2, binary, 1, b'', 2, b'a\0', '-inf', 'first')
        check(2, 'ZADD', 'zmpop-second', 0, 'other', 'inf', 'infinity')
        check(1, 'PEXPIREAT', key, deadline)
        # Request order determines the selected key, not the global best score.
        check([key, [[b'first', b'-inf'], [b'', b'1']]], 'ZMPOP', 3, 'zmpop-absent', key, 'zmpop-second', 'MIN', 'COUNT', 2)
        check(deadline, 'PEXPIRETIME', key)
        check(2, 'ZCARD', 'zmpop-second')
        check([key, [[b'a\0', b'2']]], 'ZMPOP', 3, key, key, 'zmpop-second', 'MAX')
        check([key, [[binary, b'2']]], 'ZMPOP', 2, key, 'zmpop-second', 'MIN', 'COUNT', 9223372036854775807)
        check(b'none', 'TYPE', key)
        check([b'zmpop-second', [[b'infinity', b'inf'], [b'other', b'0']]], 'ZMPOP', 2, key, 'zmpop-second', 'max', 'count', 10)
        check(None, 'ZMPOP', 2, key, 'zmpop-second', 'MAX')
        check(1, 'ZADD', 'zmpop-expired', 1, 'old')
        check(1, 'PEXPIREAT', 'zmpop-expired', 1)
        check(1, 'ZADD', key, '1.5', b'')
        check([key, [[b'', b'1.5']]], 'ZMPOP', 2, 'zmpop-expired', key, 'MIN')
        check(b'OK', 'SET', 'zmpop-wrong', 'string')
        check(1, 'ZADD', key, 7, 'live')
        check([key, [[b'live', b'7']]], 'ZMPOP', 2, key, 'zmpop-wrong', 'MAX')
        check(1, 'ZADD', key, 7, 'live')
        error(b'WRONGTYPE ', 'ZMPOP', 2, 'zmpop-wrong', key, 'MIN')
        check([b'live', b'7'], 'ZRANGE', key, 0, -1, 'WITHSCORES')
        for command, args in [('HSET', ('f', 'v')), ('SADD', ('v',)), ('RPUSH', ('v',))]:
            c.call('DEL', 'zmpop-wrong-type'); c.call(command, 'zmpop-wrong-type', *args)
            error(b'WRONGTYPE ', 'ZMPOP', 2, 'zmpop-absent', 'zmpop-wrong-type', 'MIN')
        for numkeys in ('bad', '01', '0', '-1', '-9223372036854775808', '9223372036854775808'):
            error(b'ERR numkeys should be greater than 0', 'ZMPOP', numkeys, key, 'MIN')
        error(b'ERR syntax error', 'ZMPOP', 9223372036854775807, key, 'MIN')
        for args in [(2, key, 'MIN'), (1, key, 'LEFT'), (1, key, 'MIN', 'COUNT'),
                     (1, key, 'MIN', 'WITHSCORES'), (1, key, 'MIN', 'COUNT', 1, 'COUNT', 2),
                     (1, key, 'MIN', 'COUNT', 1, 'UNKNOWN')]:
            error(b'ERR syntax error', 'ZMPOP', *args)
        for count in ('bad', '01', '0', '-1', '-9223372036854775808', '9223372036854775808'):
            error(b'ERR count should be greater than 0', 'ZMPOP', 1, 'zmpop-wrong', 'MIN', 'COUNT', count)
        for args in [(), (1,), (1, key)]: error(b'ERR wrong number of arguments', 'ZMPOP', *args)
        rng = random.Random(192491)
        names = [b'zmpop-model:a', b'zmpop-model:b', b'zmpop-model:c']
        model = {name: {} for name in names}
        for step in range(260):
            name = rng.choice(names)
            if step % 3 != 0:
                member = bytes([rng.randrange(20)]) + b'\0'
                score = rng.randrange(-7, 8)
                check(int(member not in model[name]), 'ZADD', name, score, member)
                model[name][member] = score
                check(1, 'PEXPIREAT', name, deadline)
            else:
                order = [rng.choice(names) for _ in range(5)]
                maximum, count = bool(rng.randrange(2)), rng.randrange(1, 8)
                selected = next((k for k in order if model[k]), None)
                expected = None
                if selected is not None:
                    values = sorted(model[selected].items(), key=lambda item: (item[1], item[0]), reverse=maximum)[:count]
                    expected = [selected, [[m, str(s).encode()] for m, s in values]]
                    for member, score in values: del model[selected][member]
                check(expected, 'ZMPOP', len(order), *order, 'MAX' if maximum else 'MIN', 'COUNT', count)
            for name in names:
                expected = [part for m, s in sorted(model[name].items(), key=lambda item: (item[1], item[0])) for part in (m, str(s).encode())]
                check(expected, 'ZRANGE', name, 0, -1, 'WITHSCORES')
                check(deadline if model[name] else -2, 'PEXPIRETIME', name)
        populations = [333, 307]
        expected_scores = {}
        for index, size in enumerate(populations):
            values = {b'pop:%d:%03d\0' % (index, i): str(i % 9).encode() for i in range(size)}
            expected_scores.update(values)
            check(size, 'ZADD', 'zmpop-race:%d' % index, *[part for m, s in values.items() for part in (s, m)])
        start = threading.Barrier(8)
        def worker(index):
            client = Client(port)
            popped, count = [], 0
            keys = [b'zmpop-race:0', b'zmpop-race:1']
            if index % 2: keys.reverse()
            try:
                start.wait(timeout=10)
                while True:
                    reply = client.call('ZMPOP', 3, keys[0], keys[0], keys[1], 'MAX' if index % 2 else 'MIN', 'COUNT', 3)
                    count += 1
                    if reply is None: break
                    assert isinstance(reply, list) and reply[0] in keys and 1 <= len(reply[1]) <= 3, reply
                    pairs = reply[1]
                    assert all(len(pair) == 2 and expected_scores[pair[0]] == pair[1] for pair in pairs), reply
                    assert pairs == sorted(pairs, key=lambda pair: (float(pair[1]), pair[0]), reverse=bool(index % 2)), reply
                    popped.extend(pair[0] for pair in pairs)
                return popped, count
            finally: client.close()
        with ThreadPoolExecutor(max_workers=8) as pool: results = list(pool.map(worker, range(8)))
        popped = [m for values, count in results for m in values]
        assert len(popped) == len(set(popped)) == 640 and set(popped) == set(expected_scores)
        checks += sum(count for values, count in results) + 1
        check(b'none', 'TYPE', 'zmpop-race:0')
        check(b'none', 'TYPE', 'zmpop-race:1')
    finally: c.close()
    return checks


def run_zset_sampling(port):
    c = Client(port)
    checks = 0
    def check(expected, *args):
        nonlocal checks
        actual = c.call(*args)
        assert type(actual) is type(expected) and actual == expected, (args, actual, expected)
        checks += 1
    def error(prefix, *args):
        nonlocal checks
        actual = c.call(*args)
        assert isinstance(actual, ErrorReply) and actual.startswith(prefix), (args, actual)
        checks += 1
    def sampled(key, model, count=None, scores=False):
        nonlocal checks
        args = ('ZRANDMEMBER', key) + (() if count is None else (count,)) + (('WITHSCORES',) if scores else ())
        actual = c.call(*args)
        if count is None: assert actual in model, (args, actual)
        else:
            assert isinstance(actual, list) and all(isinstance(item, bytes) for item in actual), (args, actual)
            if scores:
                assert len(actual) % 2 == 0, actual
                members = actual[::2]
                for member, score in zip(members, actual[1::2]): assert model[member] == score, (member, score)
            else: members = actual
            assert len(members) == (min(count, len(model)) if count >= 0 else -count), (args, actual)
            assert all(member in model for member in members), (args, actual)
            if count >= 0: assert len(set(members)) == len(members), (args, actual)
        checks += 1
    try:
        check(None, 'ZRANDMEMBER', 'zrandom-missing')
        for count in (0, 1, -1, 9223372036854775807): check([], 'ZRANDMEMBER', 'zrandom-missing', count)
        check([], 'ZRANDMEMBER', 'zrandom-missing', -4611686018427387903, 'WITHSCORES')
        check(1, 'ZADD', 'zrandom-one', '1.5', b'')
        check(b'', 'ZRANDMEMBER', 'zrandom-one')
        check([b'', b'', b''], 'ZRANDMEMBER', 'zrandom-one', -3)
        check([b'', b'1.5'] * 3, 'ZRANDMEMBER', 'zrandom-one', -3, 'WITHSCORES')
        model = {b'': b'1.5', bytes(range(256)): b'-inf', b'\xff': b'inf', b'a\0': b'2', b'last': b'0.25'}
        check(len(model), 'ZADD', 'zrandom-values', *[part for m, s in model.items() for part in (s, m)])
        check(1, 'PEXPIREAT', 'zrandom-values', 4102444800123)
        for _ in range(35):
            sampled('zrandom-values', model)
            for count in (0, 1, 3, 5, 100, -1, -7):
                sampled('zrandom-values', model, count)
                sampled('zrandom-values', model, count, True)
        sampled('zrandom-values', model, 9223372036854775807)
        check(4102444800123, 'PEXPIRETIME', 'zrandom-values')
        actual = c.call('ZRANGE', 'zrandom-values', 0, -1, 'WITHSCORES')
        assert dict(zip(actual[::2], actual[1::2])) == model
        checks += 1
        large = {('zrandom-large:%03d' % i).encode() + b'\0' * 80: str(i - 150).encode() for i in range(300)}
        check(300, 'ZADD', 'zrandom-large', *[part for m, s in large.items() for part in (s, m)])
        for count in (1, 17, 250, 300, 1000, -1, -450): sampled('zrandom-large', large, count, True)

        mixed = {b'm:%02d\0' % i + b'x' * 15: b'1.5' for i in range(16)}
        mixed[b'last\0' + b'y' * 256] = b'inf'
        check(len(mixed), 'ZADD', 'zrandom-mixed', *(part for m, s in mixed.items() for part in (s, m)))
        for _ in range(8):
            sampled('zrandom-mixed', mixed, 16, True)
            sampled('zrandom-mixed', mixed, -8, True)

        key = 'zrandom-pipeline'
        values = {b'item:%d\0' % i + b'x' * 280: str(i).encode() for i in range(8)}
        check(8, 'ZADD', key, *(part for m, s in values.items() for part in (s, m)))
        check(1, 'PEXPIREAT', key, 4102444800123)
        draw = ('ZRANDMEMBER', key, 8, 'WITHSCORES')
        updates = [(m, str(1000 + i).encode()) for i, m in enumerate(values)]
        commands = [command for m, s in updates for command in (draw, ('ZADD', key, s, m), draw)]
        c.socket.sendall(b''.join(c.encode(command) for command in commands))
        for member, score in updates:
            before = c.read()
            assert len(before) == 16 and dict(zip(before[::2], before[1::2])) == values
            assert c.read() == 0
            values[member] = score
            after = c.read()
            assert len(after) == 16 and dict(zip(after[::2], after[1::2])) == values
            checks += 3
        check(4102444800123, 'PEXPIRETIME', key)

        for command, args in [('SET', ('value',)), ('HSET', ('f', 'v')), ('RPUSH', ('v',)), ('SADD', ('v',))]:
            c.call('DEL', 'zrandom-wrong'); c.call(command, 'zrandom-wrong', *args)
            for count in (None, 0, 1, -1):
                error(b'WRONGTYPE ', 'ZRANDMEMBER', 'zrandom-wrong', *((count,) if count is not None else ()))
        for key in ('zrandom-wrong', 'zrandom-missing', 'zrandom-values'):
            for count in ('bad', '01', '-0', '+1', '9223372036854775808', b'1\0tail'):
                error(b'ERR value is not an integer or out of range', 'ZRANDMEMBER', key, count)
            error(b'ERR value is out of range, value must between', 'ZRANDMEMBER', key, -9223372036854775808)
            error(b'ERR value is out of range', 'ZRANDMEMBER', key, 4611686018427387904, 'WITHSCORES')
            error(b'ERR value is out of range', 'ZRANDMEMBER', key, -4611686018427387904, 'WITHSCORES')
            error(b'ERR syntax error', 'ZRANDMEMBER', key, 1, 'WITHSCORE')
            error(b'ERR syntax error', 'ZRANDMEMBER', key, 1, 'WITHSCORES', 'WITHSCORES')
            error(b'ERR value is not an integer or out of range', 'ZRANDMEMBER', key, 'bad', 'UNKNOWN', 'extra')
        error(b'ERR value is not an integer or out of range', 'ZRANDMEMBER', 'zrandom-values', 'WITHSCORES')
        error(b'ERR wrong number of arguments', 'ZRANDMEMBER')
        check(1, 'PEXPIREAT', 'zrandom-values', 1)
        check(None, 'ZRANDMEMBER', 'zrandom-values')
        check([], 'ZRANDMEMBER', 'zrandom-values', -100, 'WITHSCORES')
        # Samples and their scores come from one owning view even while another
        # client atomically replaces the key between two disjoint populations.
        old = {b'old:%d' % i: b'1' for i in range(20)}
        new = {b'new:%d' % i: b'2' for i in range(20)}
        for key, values in [('zrandom-old', old), ('zrandom-new', new), ('zrandom-race', old)]:
            check(20, 'ZADD', key, *[part for m, s in values.items() for part in (s, m)])
        start = threading.Barrier(5)
        def worker(index):
            client = Client(port)
            try:
                start.wait(timeout=10)
                for iteration in range(60):
                    if index == 0:
                        assert client.call('ZRANGESTORE', 'zrandom-race', 'zrandom-old' if iteration % 2 else 'zrandom-new', 0, -1) == 20
                    else:
                        result = client.call('ZRANDMEMBER', 'zrandom-race', -45, 'WITHSCORES')
                        assert len(result) == 90, result
                        chosen = dict(zip(result[::2], result[1::2]))
                        population = old if result[0] in old else new
                        assert all(population[m] == s for m, s in chosen.items()), result
                return 60
            finally: client.close()
        with ThreadPoolExecutor(max_workers=5) as pool: checks += sum(pool.map(worker, range(5)))
    finally: c.close()
    return checks


def run_zset_sample_safety(port):
    c = Client(port)
    try:
        assert c.call('ZADD', 'zrandom-size-safety', 1, 'member') == 1
        assert c.call('PEXPIREAT', 'zrandom-size-safety', 4102444800123) == 1
        for count, flags in [(-9223372036854775807, ()), (-4611686018427387903, ('WITHSCORES',))]:
            assert c.call('ZRANDMEMBER', 'zrandom-size-safety', count, *flags) == ErrorReply(b'ERR requested sample is too large')
            assert c.call('ZSCORE', 'zrandom-size-safety', 'member') == b'1'
            assert c.call('PEXPIRETIME', 'zrandom-size-safety') == 4102444800123
            assert c.call('PING') == b'PONG'
        return 10
    finally: c.close()


def run_zscans(port):
    c = Client(port)
    scenarios = 0
    def check(expected, *args):
        nonlocal scenarios
        actual = c.call(*args)
        assert type(actual) is type(expected) and actual == expected, (args, actual, expected)
        scenarios += 1
    def error(prefix, *args):
        nonlocal scenarios
        actual = c.call(*args)
        assert isinstance(actual, ErrorReply) and actual.startswith(prefix), (args, actual)
        assert c.call('PING') == b'PONG'
        scenarios += 1
    def scan_all(key, options=(), portable=False, mutate=None):
        nonlocal scenarios
        cursor, seen = b'0', {}
        for page in range(10000):
            client = Client(port) if portable else c
            try: reply = client.call('ZSCAN', key, cursor, *options)
            finally:
                if portable: client.close()
            assert isinstance(reply, list) and len(reply) == 2 and isinstance(reply[0], bytes) and reply[0].isdigit(), reply
            assert isinstance(reply[1], list) and len(reply[1]) % 2 == 0 and all(isinstance(value, bytes) for value in reply[1]), reply
            for member, score in zip(reply[1][::2], reply[1][1::2]):
                assert score.lower() not in (b'nan', b'-nan', b'+nan'), reply
                float(score)
                if member in seen and mutate is None: assert seen[member] == score, (member, score, seen[member])
                seen[member] = score
            cursor = reply[0]
            if mutate: mutate(page)
            if cursor == b'0':
                scenarios += 1
                return seen
        raise AssertionError(('ZSCAN did not terminate', key))
    try:
        check([b'0', []], 'ZSCAN', 'zscan-missing', 0)
        check([b'0', []], 'ZSCAN', 'zscan-missing', 0, 'COUNT', 'bad', 'NOVALUES')
        binary = bytes(range(256))
        values = {b'': b'1.5', b'a\0': b'-2', binary: b'inf', b'\xff': b'-inf', b'score-only': b'12345', b'a[b]': b'0.25', b'a?': b'2', b'a*': b'3'}
        check(len(values), 'ZADD', 'zscan-binary', *[part for member, score in values.items() for part in (score, member)])
        check(1, 'PEXPIREAT', 'zscan-binary', 4102444800123)
        for options in [(), ('COUNT', 1), ('COUNT', 9223372036854775807), ('MATCH', '*'),
                        ('MATCH', 'no-match', 'MATCH', '*', 'COUNT', 1, 'COUNT', 100)]:
            assert scan_all('zscan-binary', options, True) == values
        for pattern, expected in [(b'', {b''}), (b'a*', {b'a\0', b'a[b]', b'a?', b'a*'}), (b'a?', {b'a\0', b'a?', b'a*'}),
                                  (b'a\\?', {b'a?'}), (b'a\\*', {b'a*'}), (b'a\0', {b'a\0'}), (b''.join(b'\\' + bytes([ch]) for ch in binary), {binary}),
                                  (b'12345', set()), (b'*?*?*', {m for m in values if len(m) >= 2})]:
            assert scan_all('zscan-binary', ('MATCH', pattern, 'COUNT', 1)) == {m: values[m] for m in expected}
        check(4102444800123, 'PEXPIRETIME', 'zscan-binary')
        # A large set forces the reference's hash-table encoding. Stable members
        # must survive cursor transfer and unrelated membership changes.
        stable = {('zscan-stable:%03d' % i).encode() + b'\0' + bytes([i % 256]) * 80: str(i - 200).encode() for i in range(400)}
        check(400, 'ZADD', 'zscan-large', *[part for member, score in stable.items() for part in (score, member)])
        check(1, 'PEXPIREAT', 'zscan-large', 4102444800123)
        def churn(page):
            assert c.call('ZREM', 'zscan-large', 'transient:%d' % (page - 1)) in (0, 1)
            assert c.call('ZADD', 'zscan-large', page, 'transient:%d' % page) == 1
        scanned = scan_all('zscan-large', ('COUNT', 1), True, churn)
        assert {m: scanned[m] for m in stable} == stable
        assert scan_all('zscan-large', ('COUNT', 7, 'MATCH', b'zscan-stable:*'), True) == stable
        # Separate connections carry independent cursors, including after keys
        # are overwritten, deleted, expired or changed to another type.
        cursor_a = cursor_b = b'0'
        seen_a, seen_b = {}, {}
        done_a = done_b = False
        for _ in range(10000):
            if not done_a:
                cursor_a, items = c.call('ZSCAN', 'zscan-large', cursor_a, 'COUNT', 1)
                seen_a.update(zip(items[::2], items[1::2])); done_a = cursor_a == b'0'
            if not done_b:
                cursor_b, items = c.call('ZSCAN', 'zscan-binary', cursor_b, 'COUNT', 2)
                seen_b.update(zip(items[::2], items[1::2])); done_b = cursor_b == b'0'
            if done_a and done_b: break
        else: raise AssertionError('independent scans did not terminate')
        assert {m: seen_a[m] for m in stable} == stable and seen_b == values
        scenarios += 1
        check(1, 'ZADD', 'zscan-replaced', 1, 'old')
        check(1, 'DEL', 'zscan-replaced')
        check([b'0', []], 'ZSCAN', 'zscan-replaced', 37, 'UNKNOWN')
        check(b'OK', 'SET', 'zscan-replaced', 'string')
        error(b'WRONGTYPE ', 'ZSCAN', 'zscan-replaced', 37, 'COUNT', 'bad')
        for command, args in [('SET', ('v',)), ('HSET', ('f', 'v')), ('SADD', ('v',)), ('RPUSH', ('v',))]:
            c.call('DEL', 'zscan-wrong')
            c.call(command, 'zscan-wrong', *args)
            error(b'WRONGTYPE ', 'ZSCAN', 'zscan-wrong', 0, 'UNKNOWN')
            error(b'ERR invalid cursor', 'ZSCAN', 'zscan-wrong', 'bad')
        for cursor in ('00', '+0', '-0', ' 0'):
            check([b'0', []], 'ZSCAN', 'zscan-missing', cursor)
            reply = c.call('ZSCAN', 'zscan-binary', cursor, 'COUNT', 9223372036854775807)
            assert reply[0] == b'0' and dict(zip(reply[1][::2], reply[1][1::2])) == values
            scenarios += 1
        for cursor in (b'', b'-1', b'x', b'1x', b'18446744073709551616'):
            error(b'ERR invalid cursor', 'ZSCAN', 'zscan-binary', cursor)
        for args in [('COUNT', 0), ('COUNT', -1), ('COUNT',), ('MATCH',), ('UNKNOWN', 1), ('COUNT', 1, 'UNKNOWN'), ('TYPE', 'zset')]:
            error(b'ERR syntax error', 'ZSCAN', 'zscan-binary', 0, *args)
        for count in ('bad', '01', '-0', '9223372036854775808'):
            error(b'ERR value is not an integer or out of range', 'ZSCAN', 'zscan-binary', 0, 'COUNT', count)
        error(b'ERR NOVALUES option can only be used in HSCAN', 'ZSCAN', 'zscan-binary', 0, 'NOVALUES')
        for args in [(), ('zscan-binary',)]: error(b'ERR wrong number of arguments', 'ZSCAN', *args)
        check(1, 'PEXPIREAT', 'zscan-binary', 1)
        check([b'0', []], 'ZSCAN', 'zscan-binary', 0, 'UNKNOWN')
        # Simultaneous score updates and scans keep each member/score pair valid;
        # a full scan covers continuously present members without snapshot claims.
        start = threading.Barrier(3)
        race_values = {('zscan-race:%03d' % i).encode(): b'0' for i in range(300)}
        check(300, 'ZADD', 'zscan-race', *[part for member, score in race_values.items() for part in (score, member)])
        def writer(index):
            client = Client(port)
            try:
                start.wait(timeout=10)
                for iteration in range(160):
                    assert client.call('ZINCRBY', 'zscan-race', 1, 'zscan-race:%03d' % (iteration % 300)) is not None
                    assert client.call('ZADD', 'zscan-race', iteration, 'transient-race:%d' % index) in (0, 1)
                return True
            finally: client.close()
        with ThreadPoolExecutor(max_workers=2) as pool:
            futures = [pool.submit(writer, i) for i in range(2)]
            start.wait(timeout=10)
            seen = scan_all('zscan-race', ('COUNT', 1), True, lambda page: None)
            assert set(seen) >= set(race_values)
            for m in race_values: assert 0 <= float(seen[m]) <= 2
            assert all(future.result() for future in futures)
        scenarios += 1
        check(4102444800123, 'PEXPIRETIME', 'zscan-large')
    finally: c.close()
    return scenarios


def run_zsets(port):
    c = Client(port)
    count = 0
    def check(expected, *args):
        nonlocal count
        actual = c.call(*args)
        assert type(actual) is type(expected) and actual == expected, (args, actual, expected)
        count += 1
    def error(prefix, *args):
        nonlocal count
        actual = c.call(*args)
        assert isinstance(actual, ErrorReply) and actual.startswith(prefix), (args, actual)
        count += 1
    key = b'zset\0' + bytes(range(256))
    binary = bytes(range(256))
    deadline = 4102444800123
    def check_zero_score():
        nonlocal count
        # The reference listpack representation canonicalizes -0 to 0, while
        # its skiplist and pkv can retain -0. Both denote an equal ZSET score.
        actual = c.call('ZSCORE', 'zset-signed-zero', 'member')
        assert actual in (b'0', b'-0') and float(actual) == 0.0, actual
        count += 1
    try:
        pipeline_key = key + b':pipeline'
        requests = [('HSET', pipeline_key, b'field', b'first'),
                    ('hset', pipeline_key, b'field', b'second'),
                    ('ZADD', pipeline_key, '1', b'member'),
                    ('HGET', pipeline_key, b'field'), ('DEL', pipeline_key),
                    ('zadd', pipeline_key, '1', b'member'),
                    ('ZADD', pipeline_key, 'bad', b'member'),
                    ('ZADD', pipeline_key, '2', b'member'),
                    ('HSET', pipeline_key, b'field', b'value'),
                    ('ZSCORE', pipeline_key, b'member'), ('DEL', pipeline_key)]
        c.socket.sendall(b''.join(c.encode(args) for args in requests))
        replies = [c.read() for _ in requests]
        assert replies[:2] == [1, 0]
        assert isinstance(replies[2], ErrorReply) and replies[2].startswith(b'WRONGTYPE ')
        assert replies[3:6] == [b'second', 1, 1]
        assert isinstance(replies[6], ErrorReply) and replies[6].startswith(b'ERR ')
        assert replies[7] == 0
        assert isinstance(replies[8], ErrorReply) and replies[8].startswith(b'WRONGTYPE ')
        assert replies[9:] == [b'2', 1]
        count += len(requests)
        requests = [('ZPOPMIN', pipeline_key), ('zpopmax', pipeline_key),
                    ('ZADD', pipeline_key, '5', b'x'),
                    ('ZPOPMIN', pipeline_key), ('ZPOPMAX', pipeline_key),
                    ('ZADD', pipeline_key, '3', b'y'),
                    ('ZPOPMAX', pipeline_key), ('ZPOPMIN', pipeline_key)]
        c.socket.sendall(b''.join(c.encode(args) for args in requests))
        assert [c.read() for _ in requests] == [
            [], [], 1, [b'x', b'5'], [], 1, [b'y', b'3'], []]
        count += len(requests)
        pairs = [part for i in range(64) for part in (i, f'm{i:03d}')]
        check(64, 'ZADD', pipeline_key, *pairs)
        requests = [('ZADD', pipeline_key, 100, 'm000'),
                    ('ZADD', pipeline_key, 101, 'm001'),
                    ('ZADD', pipeline_key, 102, 'new'),
                    ('ZADD', pipeline_key, 103, 'm001'),
                    ('ZADD', pipeline_key, 104, 'new'),
                    ('ZADD', pipeline_key, 104, 'new'),
                    ('ZADD', pipeline_key, 104, 'new'),
                    ('ZADD', pipeline_key, 105, 'new'),
                    ('ZSCORE', pipeline_key, 'new'),
                    ('ZSCORE', pipeline_key, 'm001')]
        c.socket.sendall(b''.join(c.encode(args) for args in requests))
        assert [c.read() for _ in requests] == [0, 0, 1, 0, 0, 0, 0, 0, b'105', b'103']
        count += len(requests)
        check(65, 'ZCARD', pipeline_key)
        check(1, 'DEL', pipeline_key)
        requests = [('ZADD', pipeline_key, 1, 'a'),
                    ('ZADD', pipeline_key, 2, 'b'),
                    ('ZADD', pipeline_key, 3, 'c'),
                    ('ZPOPMIN', pipeline_key), ('ZPOPMIN', pipeline_key),
                    ('ZPOPMIN', pipeline_key),
                    ('ZADD', pipeline_key, 4, 'd'),
                    ('ZADD', pipeline_key, 5, 'e'),
                    ('ZSCORE', pipeline_key, 'e')]
        c.socket.sendall(b''.join(c.encode(args) for args in requests))
        assert [c.read() for _ in requests] == [
            1, 1, 1, [b'a', b'1'], [b'b', b'2'], [b'c', b'3'], 1, 1, b'5']
        count += len(requests)
        check(1, 'DEL', pipeline_key)
        check(0, 'ZCARD', key)
        check(None, 'ZSCORE', key, b'')
        check([None, None], 'ZMSCORE', key, b'', binary)
        check([], 'ZRANGE', key, 0, -1)
        check(None, 'ZRANK', key, b'')
        check(None, 'ZREVRANK', key, b'', 'WITHSCORE')
        check([], 'ZPOPMIN', key)
        check(0, 'ZREM', key, 'missing')
        check(4, 'ZADD', key, 2, binary, 1, b'', 2, b'a\0', -1, 'first')
        check(b'zset', 'TYPE', key)
        check(4, 'ZCARD', key)
        expected = [b'first', b'', binary, b'a\0']
        check(expected, 'ZRANGE', key, 0, -1)
        check(list(reversed(expected)), 'ZREVRANGE', key, 0, -1)
        check(list(reversed(expected)), 'ZRANGE', key, 0, -1, 'REV')
        check([b'first', b'-1', b'', b'1', binary, b'2', b'a\0', b'2'], 'ZRANGE', key, 0, -1, 'WITHSCORES')
        check([b'2', None, b'1', b'2'], 'ZMSCORE', key, binary, 'missing', b'', binary)
        for i, member in enumerate(expected):
            check(i, 'ZRANK', key, member)
            check(3 - i, 'ZREVRANK', key, member)
            score = b'-1' if member == b'first' else b'1' if member == b'' else b'2'
            check([i, score], 'ZRANK', key, member, 'WITHSCORE')
        for start in (-9223372036854775808, -6, -4, -1, 0, 2, 4, 9223372036854775807):
            for end in (-9223372036854775808, -6, -4, -1, 0, 2, 4, 9223372036854775807):
                begin = max(0, start + 4 if start < 0 else start)
                finish = min(3, end + 4 if end < 0 else end)
                check(expected[begin:finish + 1] if begin <= finish else [], 'ZRANGE', key, start, end)
        check(1, 'PEXPIREAT', key, deadline)
        check(0, 'ZADD', key, 'NX', 9, binary)
        check(0, 'ZADD', key, 'XX', 9, 'missing')
        check(0, 'ZADD', key, 'GT', 1, binary)
        check(0, 'ZADD', key, 'LT', 3, binary)
        check(1, 'ZADD', key, 'CH', 'GT', 3, binary)
        check(1, 'ZADD', key, 'CH', 'LT', 2, binary)
        check(0, 'ZADD', key, 'CH', 2, binary)
        check(2, 'ZADD', key, 'CH', 3, binary, 2, binary)
        check(1, 'ZADD', key, 1, 'duplicate', 2, 'duplicate')
        check(b'2', 'ZSCORE', key, 'duplicate')
        check(b'3', 'ZADD', key, 'INCR', 1, binary)
        check(None, 'ZADD', key, 'NX', 'INCR', 1, binary)
        check(None, 'ZADD', key, 'GT', 'INCR', -1, binary)
        check(b'3.25', 'ZINCRBY', key, '.25', binary)
        check(deadline, 'PEXPIRETIME', key)
        check(0, 'ZADD', 'zset-xx-missing', 'XX', 1, 'member')
        check(None, 'ZADD', 'zset-xx-missing', 'XX', 'INCR', 1, 'member')
        check(0, 'EXISTS', 'zset-xx-missing')
        # Canonical decimal output, infinities, subnormals and hexadecimal input.
        cases = [('1.25', b'1.25'), ('1e6', b'1000000'), ('1e-7', b'1e-7'), ('1e20', b'1e+20'),
                 ('0x1p+2', b'4'), ('+inf', b'inf'), ('-inf', b'-inf'),
                 ('2.2250738585072014e-308', b'2.2250738585072014e-308'), ('5e-324', b'5e-324')]
        for i, (number, text) in enumerate(cases):
            check(1, 'ZADD', 'zset-floats', number, str(i))
            check(text, 'ZSCORE', 'zset-floats', str(i))
        check(1, 'ZADD', 'zset-signed-zero', '-0', 'member')
        check_zero_score()
        check(0, 'ZADD', 'zset-signed-zero', 'CH', 0, 'member')
        check_zero_score()
        check(b'0', 'ZINCRBY', 'zset-signed-zero', 0, 'member')
        check_zero_score()
        check(1, 'ZADD', 'zset-signed-zero-wide', '-0', binary)
        check(b'-0', 'ZSCORE', 'zset-signed-zero-wide', binary)
        check(1, 'ZADD', 'zset-type:one', 1, 'member')
        check([b'0', [b'zset-type:one']], 'SCAN', 0, 'MATCH', 'zset-type:*', 'TYPE', 'ZSET', 'COUNT', 100000)
        check(1, 'ZADD', 'zset-infinity', 'inf', 'score')
        error(b'ERR resulting score is not a number (NaN)', 'ZINCRBY', 'zset-infinity', '-inf', 'score')
        check(b'inf', 'ZSCORE', 'zset-infinity', 'score')
        check(None, 'ZADD', 'zset-infinity', 'NX', 'INCR', '-inf', 'score')
        for bad in (b'', b'nan', b'NaN', b' 1', b'1 ', b'1\0', b'1e309', b'1e-9999', b'--1'):
            error(b'ERR value is not a valid float', 'ZADD', key, 7, 'would-add', bad, 'bad')
            check(None, 'ZSCORE', key, 'would-add')
            error(b'ERR value is not a valid float', 'ZINCRBY', key, bad, binary)
            check(deadline, 'PEXPIRETIME', key)
        for options, message in [(('NX', 'XX'), b'ERR XX and NX'), (('GT', 'LT'), b'ERR GT, LT'),
                                 (('NX', 'GT'), b'ERR GT, LT'), (('NX', 'LT'), b'ERR GT, LT')]:
            error(message, 'ZADD', key, *options, 1, 'bad')
        error(b'ERR INCR option supports a single', 'ZADD', key, 'INCR', 1, 'a', 2, 'b')
        error(b'ERR syntax error', 'ZADD', key, 'NX', 'XX')
        error(b'ERR syntax error', 'ZRANGE', key, 0, -1, 'REV', 'REV')
        error(b'ERR syntax error', 'ZREVRANGE', key, 0, -1, 'REV')
        error(b'ERR syntax error', 'ZRANK', key, 'member', 'WITHSCORES')
        error(b'ERR value is not an integer or out of range', 'ZRANGE', key, 'bad', -1)
        check(b'OK', 'SET', 'zset-wrong', 'string')
        for args in [('ZADD', 'zset-wrong', 1, 'm'), ('ZINCRBY', 'zset-wrong', 1, 'm'), ('ZREM', 'zset-wrong', 'm'),
                     ('ZSCORE', 'zset-wrong', 'm'), ('ZMSCORE', 'zset-wrong', 'm'), ('ZCARD', 'zset-wrong'),
                     ('ZRANK', 'zset-wrong', 'm'), ('ZREVRANK', 'zset-wrong', 'm'), ('ZRANGE', 'zset-wrong', 0, -1),
                     ('ZREVRANGE', 'zset-wrong', 0, -1), ('ZPOPMIN', 'zset-wrong', 0), ('ZPOPMAX', 'zset-wrong')]:
            error(b'WRONGTYPE ', *args)
        error(b'ERR value is not a valid float', 'ZADD', 'zset-wrong', 'bad', 'm')
        error(b'ERR value is out of range, must be positive', 'ZPOPMIN', 'zset-wrong', -1)
        for cmd in ('ZADD', 'ZINCRBY', 'ZREM', 'ZCARD', 'ZSCORE', 'ZMSCORE', 'ZRANK', 'ZREVRANK', 'ZRANGE', 'ZREVRANGE', 'ZPOPMIN', 'ZPOPMAX'):
            error(b'ERR wrong number of arguments', cmd)
        check(1, 'ZREM', key, 'duplicate', 'duplicate', 'missing')
        check(deadline, 'PEXPIRETIME', key)
        check([None], 'MGET', key)
        error(b'WRONGTYPE ', 'GET', key)
        error(b'WRONGTYPE ', 'SADD', key, 'm')
        check(1, 'PERSIST', key)
        check(-1, 'PTTL', key)
        check(1, 'PEXPIREAT', key, 1)
        check([], 'ZRANGE', key, 0, -1)
        check(1, 'ZADD', key, 1, 'revived')
        check(-1, 'PTTL', key)
        check(1, 'ZREM', key, 'revived')
        check(b'none', 'TYPE', key)
        check(3, 'ZADD', 'zset-pop-all', 1, b'a', 1, b'b\0', 2, b'c')
        check([b'c', b'2', b'b\0', b'1', b'a', b'1'],
              'ZPOPMAX', 'zset-pop-all', 10)
        check(0, 'EXISTS', 'zset-pop-all')
        key_partial = 'zset-pop-rebuild'
        check(128, 'ZADD', key_partial, *[part for i in range(128)
                                          for part in (i, f'm{i:03d}')])
        check(1, 'PEXPIREAT', key_partial, deadline)
        check([part for i in range(127, 15, -1)
               for part in (f'm{i:03d}'.encode(), str(i).encode())],
              'ZPOPMAX', key_partial, 112)
        check([f'm{i:03d}'.encode() for i in range(16)], 'ZRANGE', key_partial, 0, -1)
        check(deadline, 'PEXPIRETIME', key_partial)
        check(1, 'DEL', key_partial)
        for cmd, expected in [('ZPOPMIN', [b'a', b'1', b'b', b'1']), ('ZPOPMAX', [b'c', b'2', b'b', b'1'])]:
            check(3, 'ZADD', key, 1, 'a', 1, 'b', 2, 'c')
            check(1, 'PEXPIREAT', key, deadline)
            check([], cmd, key, 0)
            check(expected, cmd, key, 2)
            check(deadline, 'PEXPIRETIME', key)
            remaining = [b'c', b'2'] if cmd == 'ZPOPMIN' else [b'a', b'1']
            check(remaining, cmd, key, 9223372036854775807)
            check(b'none', 'TYPE', key)
            error(b'ERR syntax error', cmd, key, 1, 'extra')
        # Exact-integer score model avoids depending on another float formatter.
        rng = random.Random(89322)
        model = {}
        tokens = [b'', binary, b'a\0', b'a', b'z']
        for step in range(200):
            operation = rng.choice(('ZADD', 'ZINCRBY', 'ZREM', 'ZPOPMIN', 'ZPOPMAX'))
            member, score = rng.choice(tokens), rng.randrange(-20, 21)
            if operation == 'ZADD':
                added = int(member not in model); model[member] = score
                check(added, operation, key, score, member)
            elif operation == 'ZINCRBY':
                model[member] = model.get(member, 0) + score
                check(str(model[member]).encode(), operation, key, score, member)
            elif operation == 'ZREM':
                removed = int(member in model); model.pop(member, None)
                check(removed, operation, key, member, member)
            else:
                ordered = sorted(model, key=lambda name: (model[name], name), reverse=operation == 'ZPOPMAX')
                take = rng.randrange(4)
                popped = ordered[:take]
                expected = [part for name in popped for part in (name, str(model.pop(name)).encode())]
                check(expected, operation, key, take)
            ordered = sorted(model, key=lambda name: (model[name], name))
            expected = [part for name in ordered for part in (name, str(model[name]).encode())]
            check(expected, 'ZRANGE', key, 0, -1, 'WITHSCORES')
            check(len(model), 'ZCARD', key)
            check(b'zset' if model else b'none', 'TYPE', key)
        check(320, 'ZADD', 'zset-pop-race', *[part for i in range(320) for part in (i % 7, 'member:%d' % i)])
    finally: c.close()
    start = threading.Barrier(8)
    def worker(number):
        client = Client(port)
        try:
            start.wait(timeout=10)
            increments = [client.call('ZINCRBY', 'zset-counter', 1, b'counter\0') for _ in range(100)]
            for i in range(30):
                assert client.call('ZADD', 'zset-shared', i % 3, 'member:%d:%d' % (number, i)) == 1
            popped, checks = [], 0
            while True:
                reply = client.call('ZPOPMIN' if number % 2 else 'ZPOPMAX', 'zset-pop-race', 2)
                checks += 1
                assert isinstance(reply, list) and len(reply) in (0, 2, 4)
                if not reply: break
                for member, score in zip(reply[::2], reply[1::2]):
                    assert score == str(int(member.split(b':')[1]) % 7).encode()
                    popped.append(member)
            return increments, popped, checks
        finally: client.close()
    with ThreadPoolExecutor(max_workers=8) as pool: results = list(pool.map(worker, range(8)))
    increments = [int(value) for values, _, _ in results for value in values]
    popped = [member for _, members, _ in results for member in members]
    assert sorted(increments) == list(range(1, 801))
    assert len(popped) == len(set(popped)) == 320 and set(popped) == {('member:%d' % i).encode() for i in range(320)}
    count += 1042 + sum(checks for _, _, checks in results)
    c = Client(port)
    try:
        check(240, 'ZCARD', 'zset-shared')
        check(b'none', 'TYPE', 'zset-pop-race')
        check(b'800', 'ZSCORE', 'zset-counter', b'counter\0')
    finally: c.close()
    return count


def run_list_safety_checks(port):
    # The local reference narrows LSET indexes from long to int internally.
    # Preserve full-width range validation instead of corrupting a wrapped index.
    client = Client(port)
    count = 0
    try:
        assert client.call('RPUSH', 'list-index-safety', 'first', 'last') == 2
        assert client.call('PEXPIREAT', 'list-index-safety', 4102444800123) == 1
        count += 2
        for index in (-9223372036854775808, 9223372036854775807, -4294967296, 4294967296):
            reply = client.call('LSET', 'list-index-safety', index, 'bad')
            assert isinstance(reply, ErrorReply) and reply == b'ERR index out of range', (index, reply)
            assert client.call('LRANGE', 'list-index-safety', 0, -1) == [b'first', b'last']
            assert client.call('PEXPIRETIME', 'list-index-safety') == 4102444800123
            count += 3
    finally: client.close()
    return count


def run_zset_ranges(port):
    c = Client(port)
    count = 0
    def check(expected, *args):
        nonlocal count
        actual = c.call(*args)
        assert type(actual) is type(expected) and actual == expected, (args, actual, expected)
        count += 1
    def error(prefix, *args):
        nonlocal count
        actual = c.call(*args)
        assert isinstance(actual, ErrorReply) and actual.startswith(prefix), (args, actual)
        count += 1
    key = 'zrange-scores'
    model = {b'negative': -float('inf'), b'low': -2, b'': 0, b'a\0': 1, b'b': 1, b'\xff': 3, b'infinite': float('inf')}
    def flat(items):
        return [part for member, score in items for part in (member, str(int(score)).encode() if abs(score) != float('inf') else (b'-inf' if score < 0 else b'inf'))]
    try:
        check(len(model), 'ZADD', key, *[part for member, score in model.items() for part in (score, member)])
        bounds = [('-inf', -float('inf'), False), ('(-inf', -float('inf'), True), ('-2', -2, False), ('(-2', -2, True),
                  ('', 0, False), ('(', 0, True), ('1', 1, False), ('(1', 1, True), ('inf', float('inf'), False), ('(inf', float('inf'), True)]
        ordered = sorted(model.items(), key=lambda item: (item[1], item[0]))
        for lower, lo, lo_open in bounds:
            for upper, hi, hi_open in bounds:
                chosen = [(m, s) for m, s in ordered if (s > lo if lo_open else s >= lo) and (s < hi if hi_open else s <= hi)]
                check(len(chosen), 'ZCOUNT', key, lower, upper)
                for reverse in (False, True):
                    items = chosen[::-1] if reverse else chosen
                    start, end = (upper, lower) if reverse else (lower, upper)
                    flags = ('BYSCORE', 'REV') if reverse else ('BYSCORE',)
                    check(flat(items), 'ZRANGE', key, start, end, *flags, 'WITHSCORES')
                    check(flat(items), 'ZREVRANGEBYSCORE' if reverse else 'ZRANGEBYSCORE', key, start, end, 'WITHSCORES')
                    for offset, limit in [(0, 0), (0, -1), (1, 2), (-1, 3), (9223372036854775807, 1), (0, -9223372036854775808)]:
                        selected = [] if offset < 0 else items[offset:] if limit < 0 else items[offset:offset + limit]
                        check([m for m, s in selected], 'ZRANGE', key, start, end, *flags, 'LIMIT', offset, limit)
        check([m for m, s in ordered], 'ZRANGE', key, 0, -1, 'LIMIT', 999, -1)
        check([m for m, s in ordered], 'ZRANGEBYSCORE', key, '-1e999', '1e999')
        check([b'', b'a\0', b'b', b'\xff', b'infinite'], 'ZRANGEBYSCORE', key, '1e-999', 'inf')
        check([b'a\0'], 'ZRANGE', key, '-inf', 'inf', 'LIMIT', 0, 1, 'BYSCORE', 'LIMIT', 3, 1)
        names = [b'', b'\0', b'a', b'a\0', b'a\xff', b'b', b'\xff', bytes(range(256))]
        lexkey = 'zrange-lex'
        check(len(names), 'ZADD', lexkey, *[part for member in names for part in (7, member)])
        lex_bounds = [b'-', b'+', b'[', b'(', b'[a', b'(a', b'[a\0', b'(a\xff', b'[\xff']
        def lex_matches(m, bound, lower):
            if bound == b'-': return lower
            if bound == b'+': return not lower
            if lower: return m >= bound[1:] if bound[:1] == b'[' else m > bound[1:]
            return m <= bound[1:] if bound[:1] == b'[' else m < bound[1:]
        for lower in lex_bounds:
            for upper in lex_bounds:
                chosen = sorted(m for m in names if lex_matches(m, lower, True) and lex_matches(m, upper, False))
                check(len(chosen), 'ZLEXCOUNT', lexkey, lower, upper)
                for reverse in (False, True):
                    selected = chosen[::-1] if reverse else chosen
                    start, end = (upper, lower) if reverse else (lower, upper)
                    check(selected, 'ZRANGE', lexkey, start, end, 'BYLEX', *(('REV',) if reverse else ()))
                    check(selected[1:3], 'ZREVRANGEBYLEX' if reverse else 'ZRANGEBYLEX', lexkey, start, end, 'LIMIT', 1, 2)
        deadline = 4102444800123
        for mode, src, start, end, flags, expected in [
                ('rank', key, 1, 3, (), ordered[1:4]),
                ('score', key, '(0', 3, ('BYSCORE', 'REV'), []),
                ('score-reverse', key, 3, '(0', ('BYSCORE', 'REV', 'LIMIT', 1, 2), [(b'b', 1), (b'a\0', 1)]),
                ('lex', lexkey, b'[a', b'(b', ('BYLEX',), [(m, 7) for m in sorted(names) if b'a' <= m < b'b'])]:
            check(b'OK', 'SET', 'zrange-dest', 'old')
            check(1, 'PEXPIREAT', 'zrange-dest', deadline)
            check(len(expected), 'ZRANGESTORE', 'zrange-dest', src, start, end, *flags)
            check(flat(sorted(expected, key=lambda item: (item[1], item[0]))), 'ZRANGE', 'zrange-dest', 0, -1, 'WITHSCORES')
            check(-1 if expected else -2, 'PTTL', 'zrange-dest')
        check(1, 'PEXPIREAT', key, deadline)
        check(7, 'ZRANGESTORE', 'zrange-dest', key, 0, -1)
        check(deadline, 'PEXPIRETIME', key)
        check(3, 'ZRANGESTORE', key, key, 1, 3)
        check(-1, 'PTTL', key)
        check(flat(ordered[1:4]), 'ZRANGE', key, 0, -1, 'WITHSCORES')
        check(0, 'ZRANGESTORE', key, 'zrange-absent', 0, -1)
        check(b'none', 'TYPE', key)
        check(1, 'ZADD', 'zrange-expired', 1, 'expired')
        check(1, 'PEXPIREAT', 'zrange-expired', 1)
        check(b'OK', 'SET', key, 'delete-me')
        check(0, 'ZRANGESTORE', key, 'zrange-expired', 0, -1)
        check(b'none', 'TYPE', key)
        check(b'OK', 'SET', 'zrange-wrong', 'value')
        check(b'OK', 'SET', 'zrange-dest', 'keep')
        error(b'WRONGTYPE ', 'ZRANGESTORE', 'zrange-dest', 'zrange-wrong', 0, -1)
        check(b'keep', 'GET', 'zrange-dest')
        for command, bounds_args in [('ZCOUNT', (0, 1)), ('ZRANGEBYSCORE', (0, 1)), ('ZREVRANGEBYSCORE', (1, 0)),
                                     ('ZLEXCOUNT', ('-', '+')), ('ZRANGEBYLEX', ('-', '+')), ('ZREVRANGEBYLEX', ('+', '-')),
                                     ('ZREMRANGEBYRANK', (0, -1)), ('ZREMRANGEBYSCORE', (0, 1)), ('ZREMRANGEBYLEX', ('-', '+'))]:
            error(b'WRONGTYPE ', command, 'zrange-wrong', *bounds_args)
            check(0 if command in ('ZCOUNT', 'ZLEXCOUNT') or command.startswith('ZREM') else [], command, 'zrange-absent', *bounds_args)
        for args in [('BYSCORE', 'BYSCORE'), ('BYSCORE', 'BYLEX'), ('REV', 'REV'), ('UNKNOWN',)]:
            error(b'ERR syntax error', 'ZRANGE', 'zrange-wrong', 'bad', 'bad', *args)
        error(b'ERR syntax error, LIMIT is only supported', 'ZRANGE', key, 'bad', -1, 'LIMIT', 0, 1)
        error(b'ERR syntax error, WITHSCORES not supported', 'ZRANGE', key, '-', '+', 'BYLEX', 'WITHSCORES')
        error(b'ERR syntax error', 'ZRANGESTORE', key, lexkey, 0, -1, 'WITHSCORES')
        error(b'ERR syntax error', 'ZREVRANGE', key, 0, -1, 'REV')
        for endpoint in ('nan', 'NaN', 'bad', '0x1p2', ' 1', '1 ', '+'):
            error(b'ERR min or max is not a float', 'ZCOUNT', 'zrange-wrong', endpoint, 'inf')
        for endpoint in ('', 'a', '++', '--', 'inf'):
            error(b'ERR min or max not valid string range item', 'ZLEXCOUNT', 'zrange-wrong', endpoint, '+')
        for command, start, end, expected in [('ZREMRANGEBYRANK', -3, -1, 3), ('ZREMRANGEBYSCORE', '(0', 1, 2),
                                               ('ZREMRANGEBYLEX', b'[a', b'(b', 3)]:
            check(8, 'ZRANGESTORE', 'zrange-delete', lexkey, 0, -1)
            if command != 'ZREMRANGEBYLEX':
                check(0, 'DEL', key)
                check(7, 'ZADD', key, *[part for member, score in model.items() for part in (score, member)])
                check(7, 'ZRANGESTORE', 'zrange-delete', key, 0, -1)
            check(1, 'PEXPIREAT', 'zrange-delete', deadline)
            check(expected, command, 'zrange-delete', start, end)
            check(deadline, 'PEXPIRETIME', 'zrange-delete')
            check(8 - expected if command == 'ZREMRANGEBYLEX' else 7 - expected, 'ZREMRANGEBYRANK', 'zrange-delete', 0, -1)
            check(b'none', 'TYPE', 'zrange-delete')
            c.call('DEL', key)
        # Store/read snapshots may only contain the complete old or new value.
        import concurrent.futures
        old = [b'old:%03d' % i for i in range(30)]
        new = [b'new:%03d' % i for i in range(30)]
        check(30, 'ZADD', 'zrange-race-source', *[part for member in old for part in (1, member)])
        check(30, 'ZADD', 'zrange-race-old', *[part for member in old for part in (1, member)])
        check(30, 'ZADD', 'zrange-race-new', *[part for member in new for part in (1, member)])
        def store_worker(index):
            client = Client(port)
            try:
                for iteration in range(35):
                    if index == 0:
                        assert client.call('ZRANGESTORE', 'zrange-race-source', 'zrange-race-new' if iteration % 2 else 'zrange-race-old', 0, -1) == 30
                    else:
                        assert client.call('ZRANGESTORE', 'zrange-race-copy', 'zrange-race-source', 0, -1) == 30
                        reply = client.call('ZRANGE', 'zrange-race-copy', 0, -1)
                        assert reply in (old, new), reply
                return 35 if index == 0 else 70
            finally: client.close()
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            count += sum(pool.map(store_worker, range(4)))
    finally: c.close()
    return count


def run_cluster(port):
    client = Client(port)
    count = 0
    def slot(key):
        start = key.find(b'{')
        if start >= 0:
            end = key.find(b'}', start + 1)
            if end > start + 1:
                key = key[start + 1:end]
        return binascii.crc_hqx(key, 0) & 16383
    keys = [b'', b'123456789', b'foo', b'foo{bar}', b'{bar}tail', b'foo{}{bar}',
            b'foo{{bar}}', b'foo{bar}{zap}', b'foo{', b'}{', b'\0',
            b'abc\0def', b'left{tag\0bytes}right', bytes(range(256))]
    rng = random.Random(20261030)
    keys += [bytes(rng.randrange(256) for _ in range(rng.randrange(0, 256))) for _ in range(200)]
    try:
        for key in keys:
            actual = client.call('CLUSTER', 'KEYSLOT', key)
            assert type(actual) is int and actual == slot(key), (key, actual, slot(key))
            count += 1
        for args in [('CLUSTER', 'KEYSLOT'), ('CLUSTER', 'KEYSLOT', 'a', 'b')]:
            reply = client.call(*args)
            assert isinstance(reply, ErrorReply) and reply.startswith(b'ERR wrong number of arguments'), reply
            count += 1
        assert client.call('SELECT', 0) == b'OK'
        count += 1
        for selected in (-1, 1, 16):
            assert client.call('SELECT', selected) == ErrorReply(b'ERR SELECT is not allowed in cluster mode')
            count += 1
        assert client.call('SCAN', 0) == [b'0', []]
        count += 1
        a, b = b'{different-one}a', b'{different-two}b'
        assert slot(a) != slot(b)
        for args in [('COPY', a, b), ('COPY', a, b, 'DB', 0, 'REPLACE'), ('RENAME', a, b), ('RENAMENX', a, b), ('MGET', a, b), ('DEL', a, b), ('EXISTS', a, b),
                     ('MSET', a, '1', b, '2'), ('MSETNX', a, '1', b, '2'),
                     ('LMOVE', a, b, 'LEFT', 'RIGHT'), ('RPOPLPUSH', a, b),
                     ('LMPOP', 2, a, b, 'LEFT', 'COUNT', 2), ('LMPOP', 3, a, a, b, 'RIGHT'),
                     ('SMOVE', a, b, 'member'), ('SUNION', a, b), ('SINTER', a, b), ('SDIFF', a, b),
                     ('SUNIONSTORE', a, a, b), ('SINTERSTORE', a, a, b), ('SDIFFSTORE', a, a, b),
                     ('SINTERCARD', 2, a, b, 'LIMIT', 1), ('ZRANGESTORE', a, b, 0, -1),
                     ('ZMPOP', 2, a, b, 'MIN'), ('ZMPOP', 3, a, a, b, 'MAX', 'COUNT', 2),
                     ('ZUNION', 2, a, b), ('ZINTER', 2, a, b), ('ZDIFF', 2, a, b), ('ZINTERCARD', 2, a, b),
                     ('ZUNIONSTORE', a, 1, b), ('ZINTERSTORE', a, 1, b), ('ZDIFFSTORE', a, 1, b),
                     ('ZUNIONSTORE', a, 2, a, b), ('ZINTERSTORE', a, 2, a, b), ('ZDIFFSTORE', a, 2, a, b)]:
            reply = client.call(*args)
            assert isinstance(reply, ErrorReply) and reply.startswith(b'CROSSSLOT '), (args, reply)
            count += 1
        for args in [('COPY', a, a), ('COPY', b'{same}a', b'{same}b', 'REPLACE'), ('RENAME', a, a), ('RENAMENX', a, a), ('RENAME', b'{same}a', b'{same}b'), ('RENAMENX', b'{same}a', b'{same}b'), ('GET', b''), ('GET', b'abc\0def'), ('MGET', b'{same}a', b'{same}b'),
                     ('MSET', b'{same}a', '1', b'{same}b', '2'),
                     ('LPUSH', a, 'v'), ('RPUSH', a, 'v'), ('LPUSHX', a, 'v'), ('RPUSHX', a, 'v'),
                     ('LPOP', a), ('RPOP', a, 1), ('LLEN', a), ('LRANGE', a, 0, -1),
                     ('LINDEX', a, 0), ('LSET', a, 0, 'v'), ('LTRIM', a, 0, -1),
                     ('LMOVE', b'{same}a', b'{same}b', 'LEFT', 'RIGHT'),
                     ('LMOVE', a, a, 'RIGHT', 'LEFT'), ('RPOPLPUSH', b'{same}a', b'{same}b'),
                     ('LMPOP', 2, b'{same}a', b'{same}b', 'RIGHT', 'COUNT', 2),
                     ('LMPOP', 1, a, 'LEFT'), ('LMPOP', 3, b'{same}a', b'{same}a', b'{same}b', 'LEFT'),
                     ('LINSERT', a, 'BEFORE', 'pivot', 'v'), ('LREM', a, 0, 'v'), ('LPOS', a, 'v'),
                     ('SADD', a, b), ('SREM', a, b), ('SCARD', a), ('SISMEMBER', a, b), ('SMISMEMBER', a, b, a), ('SMEMBERS', a),
                     ('SMOVE', a, a, b), ('SUNION', a, a), ('SINTER', a, a), ('SDIFF', a, a),
                     ('SUNIONSTORE', a, a), ('SINTERSTORE', a, a), ('SDIFFSTORE', a, a),
                     ('SINTERCARD', 2, a, a, 'LIMIT', 1), ('SPOP', a, 1), ('SRANDMEMBER', a, -1),
                     ('HRANDFIELD', a), ('HRANDFIELD', a, -2, 'WITHVALUES'),
                     ('HRANDFIELD', a, 'bad'),
                     ('HRANDFIELD', a, 1, 'BAD'),
                     ('HINCRBYFLOAT', a, 'field', '.125'),
                     ('HINCRBYFLOAT', a, 'field', 'bad'),
                     ('INCRBYFLOAT', a, '.125'),
                     ('INCRBYFLOAT', a, 'bad'),
                     ('GETBIT', a, 0), ('GETBIT', a, 'bad'),
                     ('SETBIT', a, 0, 1), ('SETBIT', a, 'bad', 'bad'),
                     ('BITCOUNT', a), ('BITCOUNT', a, 'bad', 1),
                     ('BITPOS', a, 1), ('BITPOS', a, 'bad'),
                     ('BITOP', 'XOR', a, a, a),
                     ('BITOP', 'BAD', a, a),
                     ('BITOP', 'NOT', a, a, a),
                     ('BITOP', 'ANDOR', b'{same}a', b'{same}b'),
                     ('BITFIELD', a, 'SET', 'u8', 0, 1),
                     ('BITFIELD', a, 'GET', 'u64', 'bad'),
                     ('BITFIELD_RO', a, 'SET', 'u8', 0, 1),
                     ('BITFIELD_RO', a, 'OVERFLOW', 'bad'),
                     ('HSCAN', a, 0), ('SSCAN', a, 0), ('ZSCAN', a, 0, 'COUNT', 1, 'MATCH', b),
                     ('ZMPOP', 1, a, 'MIN'), ('ZMPOP', 3, b'{same}a', b'{same}a', b'{same}b', 'MAX', 'COUNT', 2),
                     ('ZUNION', 2, a, a, 'WEIGHTS', 1, 2, 'WITHSCORES'), ('ZINTER', 2, a, a, 'AGGREGATE', 'MIN'),
                     ('ZDIFF', 2, a, a), ('ZINTERCARD', 2, a, a, 'LIMIT', 1),
                     ('ZUNIONSTORE', a, 1, a), ('ZINTERSTORE', a, 1, a), ('ZDIFFSTORE', a, 1, a),
                     ('ZRANDMEMBER', a), ('ZRANDMEMBER', a, -2, 'WITHSCORES'), ('ZADD', a, 1, b), ('ZINCRBY', a, 1, b), ('ZREM', a, b), ('ZCARD', a),
                     ('ZSCORE', a, b), ('ZMSCORE', a, b, a), ('ZRANGE', a, 0, -1), ('ZREVRANGE', a, 0, -1),
                     ('ZRANK', a, b), ('ZREVRANK', a, b), ('ZPOPMIN', a), ('ZPOPMAX', a, 1),
                     ('ZRANGEBYSCORE', a, 0, 1), ('ZREVRANGEBYSCORE', a, 1, 0),
                     ('ZRANGEBYLEX', a, '-', '+'), ('ZREVRANGEBYLEX', a, '+', '-'),
                     ('ZCOUNT', a, 0, 1), ('ZLEXCOUNT', a, '-', '+'), ('ZREMRANGEBYRANK', a, 0, -1),
                     ('ZREMRANGEBYSCORE', a, 0, 1), ('ZREMRANGEBYLEX', a, '-', '+'),
                     ('ZRANGESTORE', a, a, 0, -1), ('ZRANGESTORE', b'{same}a', b'{same}b', 0, -1)]:
            reply = client.call(*args)
            assert isinstance(reply, ErrorReply) and reply.startswith(b'CLUSTERDOWN '), (args, reply)
            count += 1
    finally:
        client.close()
    return count


def run_command_name_safety(port):
    client = Client(port)
    try:
        for command in [('GET', 'smoke'), ('MULTI',), ('EXEC',),
                        ('DISCARD',), ('WATCH', 'smoke'), ('UNWATCH',)]:
            name = command[0].encode() + b'\0suffix'
            reply = client.call(name, *command[1:])
            assert isinstance(reply, ErrorReply) and reply.startswith(
                b'ERR unknown command'), (command, reply)
            assert client.call('PING') == b'PONG'
    finally:
        client.close()
    return 12


def run_overflow_checks(port):
    # Some reference builds optimize away their signed-overflow check in the
    # relative-millisecond expiry path. These checks intentionally validate pkv
    # independently; the difference is documented rather than accepted as parity.
    client = Client(port)
    count = 0
    try:
        for args in [('PSETEX', 'overflow-expiry', '9223372036854775807', 'bad'),
                     ('SET', 'overflow-expiry', 'bad', 'PX', '9223372036854775807')]:
            reply = client.call(*args)
            assert isinstance(reply, ErrorReply) and reply.startswith(b'ERR invalid expire time'), reply
            assert client.call('GET', 'overflow-expiry') is None
            count += 2
        assert client.call('SET', 'overflow-getex', 'preserved') == b'OK'
        reply = client.call('GETEX', 'overflow-getex', 'PX', '9223372036854775807')
        assert isinstance(reply, ErrorReply) and reply.startswith(b'ERR invalid expire time'), reply
        assert client.call('GET', 'overflow-getex') == b'preserved'
        assert client.call('PTTL', 'overflow-getex') == -1
        count += 4
    finally:
        client.close()
    return count


def run_zset_float_differential(pkv_binary, redis_binary):
    # Equal-cardinality source order is determined by the platform qsort used by
    # the reference. Compare live endpoints rather than assuming a stable sort.
    rng = random.Random(710124)
    comparisons = 0
    with server(os.path.abspath(pkv_binary), False, False) as pkv_port, server(os.path.abspath(redis_binary), True, False) as redis_port:
        pkv, redis = Client(pkv_port), Client(redis_port)
        try:
            for case in range(300):
                count = rng.randrange(1, 36)
                keys = ['float-tie:%d' % i for i in range(count)]
                scores = []
                for key in keys:
                    score = rng.choice([1e16, -1e16, 1, -1, 1e300, -1e300, 0])
                    scores.append(score)
                    assert pkv.call('ZADD', key, score, 'x') in (0, 1)
                    assert redis.call('ZADD', key, score, 'x') in (0, 1)
                for command in ('ZUNION', 'ZINTER'):
                    actual = pkv.call(command, count, *keys, 'WITHSCORES')
                    expected = redis.call(command, count, *keys, 'WITHSCORES')
                    # Retain the existing disclosed compact negative-zero exception.
                    equivalent_zero = actual in ([b'x', b'0'], [b'x', b'-0']) and expected in ([b'x', b'0'], [b'x', b'-0'])
                    assert actual == expected or equivalent_zero, (case, command, scores, actual, expected)
                    comparisons += 1
        finally:
            pkv.close(); redis.close()
    return comparisons


def run_inline_pipeline(port):
    client = Client(port)
    frames = [
        (b'PING\r\n', b'PONG'),
        (b' \tSET inline:key value \t\r\n', b'OK'),
        (Client.encode(('GET', 'inline:key')), b'value'),
        (b'PING payload\n', b'payload'),
        (Client.encode(('ECHO', b'a\0b')), b'a\0b'),
        (b'SET inline:number 40\r\n', b'OK'),
        (b'INCR inline:number\r\n', 41),
        (b'SET "inline:quoted key" "two words"\r\n', b'OK'),
        (Client.encode(('GET', 'inline:quoted key')), b'two words'),
        (b"PING 'hello world'\r\n", b'hello world'),
    ]
    wire = b''.join(frame for frame, _ in frames)
    count = 0
    try:
        for chunk in (1, 2, 7, 64, len(wire)):
            for pos in range(0, len(wire), chunk):
                client.socket.sendall(wire[pos:pos + chunk])
            for frame, expected in frames:
                actual = client.read()
                assert type(actual) is type(expected) and actual == expected, (
                    frame, actual, expected)
                count += 1
    finally:
        client.close()
    return count


def run_wide_pipeline(port):
    client = Client(port)
    try:
        key = 'pipeline-zadd-cache'
        members = [f'member:{i:03d}' for i in range(128)]
        pairs = [part for member in members for part in (0, member)]
        assert client.call('ZADD', key, *pairs) == len(members)
        first, second = members[:2]
        updates = [(0, first), (0, second), (0, first), (9, first),
                   (0, second), (0, first), (0, second), (0, first)]
        client.socket.sendall(b''.join(
            client.encode(('ZADD', key, score, member))
            for score, member in updates))
        assert [client.read() for _ in updates] == [0] * len(updates)
        # Reapplying the old score after a write must not use a stale hit.
        assert client.call('ZSCORE', key, first) == b'0'
        assert client.call('ZCARD', key) == len(members)

        fields = [f'field:{i:014d}' for i in range(1536)]
        values = [f'value:{i:054d}' for i in range(len(fields))]
        pairs = [part for pair in zip(fields, values) for part in pair]
        assert client.call('HSET', 'wide-pipeline', *pairs) == len(fields)
        names = fields[::2] + fields[1::2]
        expected = [value.encode() for value in values[::2] + values[1::2]]
        request = client.encode(('HMGET', 'wide-pipeline', *names))
        # Fill the client's send window before reading any reply, as
        # redis-benchmark does. Both directions exceed the TCP buffers.
        client.socket.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 32768)
        client.socket.settimeout(5)
        client.socket.sendall(request * 64 + client.encode(('PING',)))
        client.socket.shutdown(socket.SHUT_WR)
        for _ in range(64):
            assert client.read() == expected
        assert client.read() == b'PONG'
    finally:
        client.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--pkv', required=True)
    parser.add_argument('--redis', help='optional redis-server binary for the same compatibility checks')
    parser.add_argument('--cluster-only', action='store_true')
    args = parser.parse_args()
    for binary, is_redis in ((args.pkv, False), (args.redis, True)):
        if binary:
            with server(os.path.abspath(binary), is_redis, args.cluster_only) as port:
                count = run_cluster(port) if args.cluster_only else run(port)
                print('%s: %d %s assertions passed' % ('Redis' if is_redis else 'pkv', count, 'cluster routing' if args.cluster_only else 'socket'))
                if not args.cluster_only:
                    run_wide_pipeline(port)
                    print('%s: %d mixed inline pipeline assertions passed' %
                          ('Redis' if is_redis else 'pkv',
                           run_inline_pipeline(port)))
                    print('%s: %d list assertions passed' % ('Redis' if is_redis else 'pkv', run_lists(port)))
                    print('%s: %d list transaction assertions passed' % ('Redis' if is_redis else 'pkv', run_list_transactions(port)))
                    print('%s: %d list edit/search assertions passed' % ('Redis' if is_redis else 'pkv', run_list_edit_search(port)))
                    print('%s: %d set assertions passed' % ('Redis' if is_redis else 'pkv', run_sets(port)))
                    print('%s: %d set sample/cardinality assertions passed' % ('Redis' if is_redis else 'pkv', run_set_sampling(port)))
                    print('%s: %d scan scenarios passed' % ('Redis' if is_redis else 'pkv', run_scans(port)))
                    print('%s: %d sorted set scan scenarios passed' % ('Redis' if is_redis else 'pkv', run_zscans(port)))
                    print('%s: %d sorted set assertions passed' % ('Redis' if is_redis else 'pkv', run_zsets(port)))
                    print('%s: %d sorted set range assertions passed' % ('Redis' if is_redis else 'pkv', run_zset_ranges(port)))
                    print('%s: %d sorted set sampling assertions passed' % ('Redis' if is_redis else 'pkv', run_zset_sampling(port)))
                    print('%s: %d sorted set multi-pop assertions passed' % ('Redis' if is_redis else 'pkv', run_zmpops(port)))
                    print('%s: %d sorted set algebra assertions passed' % ('Redis' if is_redis else 'pkv', run_zset_algebra(port)))
                    print('%s: %d rename assertions passed' % ('Redis' if is_redis else 'pkv', run_renames(port)))
                    print('%s: %d logical DB/copy assertions passed' % ('Redis' if is_redis else 'pkv', run_logical_databases(port)))
                if not is_redis and not args.cluster_only:
                    print('pkv: %d command name safety checks passed' %
                          run_command_name_safety(port))
                    print('pkv: %d expiry overflow assertions passed' % run_overflow_checks(port))
                    print('pkv: %d list index safety assertions passed' % run_list_safety_checks(port))
                    print('pkv: %d sample size safety assertions passed' % run_sample_size_checks(port))
                    print('pkv: %d scan cursor safety assertions passed' % run_scan_safety_checks(port))
                    print('pkv: %d sorted set sample size safety assertions passed' % run_zset_sample_safety(port))
            if not args.cluster_only:
                print('%s: %d logical DB configuration assertions passed' % ('Redis' if is_redis else 'pkv', run_database_configuration(binary, is_redis)))
    if args.redis and not args.cluster_only:
        print('pkv/Redis: %d floating aggregation differential comparisons passed' % run_zset_float_differential(args.pkv, args.redis))


if __name__ == '__main__':
    main()
