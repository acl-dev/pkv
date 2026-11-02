#!/usr/bin/env python3
"""Blocking lists: protocol, queue ordering, cancellation and native storage."""
import argparse
import contextlib
import os
import tempfile
import time
from integration_test import Client, ErrorReply, ports, server
from resp3_client import RespClient, canonical


def wait_count(admin, count):
    deadline = time.monotonic() + 5
    expected = ('blocked_clients:%d\r\n' % count).encode()
    while time.monotonic() < deadline:
        if expected in admin.call('INFO', 'clients'):
            return
        time.sleep(.002)
    raise AssertionError((count, admin.call('CLIENT', 'LIST')))


def commands(timeout):
    return [('BLPOP', 'a', 'b', timeout), ('BRPOP', 'a', 'b', timeout),
            ('BLMPOP', timeout, 2, 'a', 'b', 'LEFT', 'COUNT', 2),
            ('BLMOVE', 'a', 'destination', 'LEFT', 'RIGHT', timeout),
            ('BRPOPLPUSH', 'a', 'destination', timeout)]


def compare(clients):
    checks = 0

    def call(*args):
        nonlocal checks
        a, b = [client.call(*args) for client in clients]
        assert type(a) is type(b) and a == b, (args, a, b)
        checks += 1
        return a

    for value in ['', 'nan', '+inf', '-inf', '1e309', '-1', '-0',
                  '-0.00001', '-0.001', '0', '0.0001', '1e-310',
                  '1e-9999', '0x1p0', ' 1', '1 ', b'1\0tail',
                  '9223372036854775807', '9223372036854.776']:
        for args in commands(value):
            call('MULTI')
            call(*args)
            call('EXEC')
    malformed = [('BLPOP',), ('BRPOP', 'a'),
                 ('BLMOVE', 'a', 'b', 'LEFT', 'RIGHT', 0, 'extra'),
                 ('BRPOPLPUSH', 'a', 'b', 0, 'extra')]
    for number in ['0', '-1', '01', '+1', '1.5', 'nan',
                   '9223372036854775807', '9223372036854775808']:
        malformed.append(('BLMPOP', 'bad', number, 'a', 'LEFT'))
    for direction in ['wrong', b'LEFT\0tail', 'right']:
        malformed.append(('BLMPOP', 0, 1, 'a', direction))
        malformed.append(('BLMOVE', 'a', 'b', direction, 'LEFT', 0))
    for count in ['0', '-1', '01', 'nan', '9223372036854775807']:
        malformed.append(('BLMPOP', 0, 1, 'a', 'RIGHT', 'COUNT', count))
    malformed += [('BLMPOP', 0, 1, 'a', 'LEFT', 'COUNT'),
                  ('BLMPOP', 0, 1, 'a', 'LEFT', 'COUNT', 1, 'COUNT', 2),
                  ('BLMPOP', 0, 1, 'a', 'LEFT', b'COUNT\0x', 2)]
    for args in malformed:
        call('MULTI')
        call(*args)
        call('EXEC')
    for reason in ['TIMEOUT', 'ERROR', 'bad', b'ERROR\0suffix']:
        call('CLIENT', 'UNBLOCK', -1, reason)
        call('CLIENT', 'UNBLOCK', 'invalid', reason)
    call('CLIENT', 'UNBLOCK', -1, 'TIMEOUT', 'extra')
    for args in commands(0):
        call('DEL', 'a', 'b', 'destination')
        call('RPUSH', 'b', b'one\0', b'v' * 40000, 'three')
        call('MULTI')
        call(*args)
        call('EXEC')
        call('LRANGE', 'b', 0, -1)
        call('LRANGE', 'destination', 0, -1)
    call('SET', 'a', 'wrong')
    for args in commands('0.001'):
        call(*args)
    call('DEL', 'a', 'b', 'destination')
    for args in commands(0):
        call('COMMAND', 'GETKEYS', *args)
        call('COMMAND', 'GETKEYSANDFLAGS', *args)
        call('COMMAND', 'INFO', args[0])
    return checks


def typed(endpoints):
    clients = [RespClient(port) for port in endpoints]
    admins = [Client(port) for port in endpoints]
    checked = 0
    try:
        for protocol in [2, 3]:
            for client in clients:
                client.call('HELLO', protocol)
            for args in commands('.001'):
                replies = [client.call(*args) for client in clients]
                assert canonical(replies[0]) == canonical(replies[1]), replies
                checked += 1
            for args in commands(0):
                for admin, client in zip(admins, clients):
                    admin.call('DEL', 'a', 'b', 'destination')
                    client.socket.sendall(Client.encode(args))
                    wait_count(admin, 1)
                    admin.call('RPUSH', 'a', 'first', 'second')
                replies = [client.read() for client in clients]
                assert canonical(replies[0]) == canonical(replies[1]), replies
                checked += 1
    finally:
        for client in clients:
            client.close()
        for admin in admins:
            admin.close()
    return checked


def asynchronous(port):
    admin, worker = Client(port), Client(port)
    observations = []
    try:
        identity = worker.call('CLIENT', 'ID')
        for args in commands('.005'):
            observations.append(worker.call(*args))
            observations.append(worker.call('PING'))
        for reason in ['TIMEOUT', 'ERROR']:
            worker.socket.sendall(Client.encode(['BRPOP', 'x', 0]))
            wait_count(admin, 1)
            observations.append(admin.call('CLIENT', 'UNBLOCK', identity,
                                           reason))
            observations.append(worker.read())
            observations.append(worker.call('PING'))
        worker.socket.sendall(Client.encode(['PING']) +
                              Client.encode(['BLPOP', 'x', 0]) +
                              Client.encode(['SET', 'after', 'yes']))
        observations.append(worker.read())
        wait_count(admin, 1)
        observations.append(admin.call('GET', 'after'))
        admin.call('RPUSH', 'x', 'payload')
        observations.extend([worker.read(), worker.read()])
        for args in commands(0):
            admin.call('DEL', 'a', 'b', 'destination')
            worker.socket.sendall(Client.encode(args))
            wait_count(admin, 1)
            observations.append(b'b' in worker_name(admin, identity)[b'flags'])
            admin.call('RPUSH', 'a', 'one', 'two', 'three')
            observations.append(worker.read())
            observations.append(admin.call('LRANGE', 'a', 0, -1))
            observations.append(admin.call('LRANGE', 'destination', 0, -1))
        admin.call('DEL', 'large', 'pending')
        worker.socket.sendall(Client.encode(['BLPOP', 'pending', 0]))
        wait_count(admin, 1)
        worker.socket.sendall(Client.encode(['SET', 'large', b'z' * 40000]))
        admin.call('RPUSH', 'pending', 'go')
        observations.extend([worker.read(), worker.read(),
                             admin.call('STRLEN', 'large')])
        admin.call('DEL', 'source', 'bad-target')
        admin.call('SET', 'bad-target', 'text')
        worker.socket.sendall(Client.encode(
            ['BLMOVE', 'source', 'bad-target', 'LEFT', 'RIGHT', 0]))
        wait_count(admin, 1)
        admin.call('RPUSH', 'source', 'preserved')
        observations.append(worker.read())
        observations.append(admin.call('LRANGE', 'source', 0, -1))
        worker.socket.sendall(Client.encode(['BLPOP', 'abandoned', 0]) +
                              Client.encode(['SET', 'unexecuted', 'bad']))
        wait_count(admin, 1)
        worker.close()
        wait_count(admin, 0)
        observations.append(admin.call('GET', 'unexecuted'))
    finally:
        worker.close()
        admin.close()
    return observations


def worker_name(admin, identity):
    row = admin.call('CLIENT', 'LIST', 'ID', identity).strip()
    return dict(item.split(b'=', 1) for item in row.split())


def ordering(port):
    admin = Client(port)
    workers = [Client(port) for _ in range(16)]
    observations = []
    try:
        for i, worker in enumerate(workers):
            worker.socket.sendall(Client.encode(['BLPOP', 'fifo', 0]))
            wait_count(admin, i + 1)
        admin.call('RPUSH', 'fifo', *range(16))
        observations.append([worker.read() for worker in workers])
        for case in range(3):
            admin.call('DEL', 'a', 'b')
            workers[0].socket.sendall(Client.encode(['BLPOP', 'a', 'b', 0]))
            wait_count(admin, 1)
            admin.call('MULTI')
            if case == 0:
                admin.call('SET', 'a', 'wrong')
            else:
                admin.call('RPUSH', 'a', 'old')
            admin.call('RPUSH', 'b', 'b')
            admin.call('DEL', 'a')
            if case != 2:
                admin.call('RPUSH', 'a', 'new')
            admin.call('EXEC')
            observations.append(workers[0].read())
        for _ in range(8):
            admin.call('DEL', 'a', 'b')
            workers[0].socket.sendall(Client.encode(['BLPOP', 'a', '.04']))
            wait_count(admin, 1)
            workers[1].socket.sendall(Client.encode(['BLPOP', 'a', 'b', '.1']))
            wait_count(admin, 2)
            admin.call('MULTI')
            admin.call('RPUSH', 'b', 'b')
            admin.call('RPUSH', 'a', 'a')
            admin.call('EXEC')
            observations.append((workers[0].read(), workers[1].read(),
                                 admin.call('LRANGE', 'b', 0, -1)))
    finally:
        for worker in workers:
            worker.close()
        admin.close()
    return observations


def permissions(port):
    admin, worker = Client(port), Client(port)
    observations = []
    try:
        admin.call('ACL', 'SETUSER', 'blocked', 'on', 'nopass',
                   '+blpop', '~allowed:*')
        assert worker.call('AUTH', 'blocked', 'unused') == b'OK'
        observations.append(worker.call('BLPOP', 'forbidden', 0))
        worker.socket.sendall(Client.encode(['BLPOP', 'allowed:list', 0]))
        wait_count(admin, 1)
        admin.call('ACL', 'SETUSER', 'blocked', '-blpop')
        admin.call('RPUSH', 'allowed:list', 'preserved')
        observations.append(worker.read())
        observations.append(admin.call('LRANGE', 'allowed:list', 0, -1))
    finally:
        admin.close()
        worker.close()
    return observations


def watches_and_databases(port):
    admin, worker, watcher = Client(port), Client(port), Client(port)
    observations = []
    try:
        admin.call('RPUSH', 'watched-target', 'old')
        watcher.call('WATCH', 'watched-target')
        watcher.call('MULTI')
        watcher.call('PING')
        worker.socket.sendall(Client.encode(
            ['BLMOVE', 'watched-source', 'watched-target', 'LEFT', 'RIGHT', 0]))
        wait_count(admin, 1)
        admin.call('RPUSH', 'watched-source', 'new')
        observations.append(worker.read())
        observations.append(watcher.call('EXEC'))
        watcher.call('WATCH', 'untouched')
        watcher.call('MULTI')
        watcher.call('PING')
        observations.append(worker.call('BLPOP', 'untouched', '.003'))
        observations.append(watcher.call('EXEC'))
        worker.call('SELECT', 3)
        worker.socket.sendall(Client.encode(['BLPOP', 'scoped', 0]))
        wait_count(admin, 1)
        admin.call('RPUSH', 'scoped', 'wrong-db')
        wait_count(admin, 1)
        admin.call('SELECT', 3)
        admin.call('RPUSH', 'scoped', 'right-db')
        observations.append(worker.read())
        admin.call('SELECT', 0)
        observations.append(admin.call('LRANGE', 'scoped', 0, -1))
        worker.call('SELECT', 0)
        identity = worker.call('CLIENT', 'ID')
        worker.socket.sendall(Client.encode(['BLPOP', 'kill-me', 0]))
        wait_count(admin, 1)
        observations.append(admin.call('CLIENT', 'KILL', 'ID', identity))
        try:
            worker.read()
            raise AssertionError('killed client received a reply')
        except (OSError, AssertionError) as error:
            assert 'killed client received' not in str(error)
        wait_count(admin, 0)
    finally:
        watcher.close()
        admin.close()
        worker.close()
    return observations


def policy(binary, reference):
    options = dict(redis=reference)
    if not reference:
        options['replica_rpc_source'] = '127.0.0.1:%d' % ports()
    with server(binary, **options) as port:
        admin, worker = Client(port), Client(port)
        try:
            if reference:
                admin.call('REPLICAOF', '127.0.0.1', ports())
            admin.call('CONFIG', 'SET', 'replica-read-only', 'no')
            worker.socket.sendall(Client.encode(['BLPOP', 'policy', 0]))
            wait_count(admin, 1)
            admin.call('MULTI')
            admin.call('CONFIG', 'SET', 'replica-read-only', 'yes')
            admin.call('RPUSH', 'policy', 'preserved')
            committed = admin.call('EXEC')
            result = worker.read()
            return committed, result, admin.call('LRANGE', 'policy', 0, -1)
        finally:
            admin.close()
            worker.close()


def cluster(binary, reference):
    with server(binary, redis=reference, cluster=True) as port:
        client = Client(port)
        try:
            return [client.call(*args) for args in
                    [('BLPOP', '{a}x', '{b}x', 0),
                     ('BRPOP', '{a}x', 0),
                     ('BLMPOP', 0, 2, '{a}x', '{b}x', 'LEFT'),
                     ('BLMOVE', '{a}x', '{b}x', 'LEFT', 'RIGHT', 0),
                     ('BRPOPLPUSH', '{a}x', '{b}x', 0),
                     ('BLPOP', '{a}x', '{b}x', 'bad'),
                     ('BLMOVE', '{a}x', '{b}x', 'bad', 'RIGHT', 0),
                     ('BLMPOP', 'bad', 0, '{a}x', 'LEFT')]]
        finally:
            client.close()


def persistence(binary, reference, backend):
    with tempfile.TemporaryDirectory(prefix='pkv-blocking-restart-') as root:
        options = dict(redis=reference, backend=backend,
                       storage_path=None if reference else root)
        with server(binary, **options) as port:
            client = Client(port)
            client.call('SELECT', 3)
            client.call('RPUSH', 'persist', 'a', b'b' * 40000)
            assert client.call('BRPOPLPUSH', 'persist', 'target', 0) == (
                b'b' * 40000)
            client.close()
        if not reference and backend in ('rdb', 'wdb'):
            with server(binary, **options) as port:
                client = Client(port)
                client.call('SELECT', 3)
                assert client.call('LRANGE', 'persist', 0, -1) == [b'a']
                assert client.call('LRANGE', 'target', 0, -1) == [b'b' * 40000]
                client.close()


def replication(binary, backend):
    with server(binary, backend=backend) as source:
        with server(binary, backend=backend, replica_rpc_source=(
                '127.0.0.1:%d' % (source + 10000))) as replica:
            primary, admin, worker = Client(source), Client(replica), (
                Client(replica))
            try:
                deadline = time.monotonic() + 8
                while True:
                    primary.call('SET', 'link-ready', 'yes')
                    if admin.call('GET', 'link-ready') == b'yes':
                        break
                    assert time.monotonic() < deadline
                    time.sleep(.02)
                admin.call('CONFIG', 'SET', 'replica-read-only', 'no')
                worker.socket.sendall(Client.encode(['BLPOP', 'a', 'b', 0]))
                wait_count(admin, 1)
                payload = b'r' * 40000
                primary.call('MULTI')
                primary.call('SET', 'a', 'wrong')
                primary.call('RPUSH', 'b', 'preserved')
                primary.call('DEL', 'a')
                primary.call('RPUSH', 'a', payload)
                assert primary.call('EXEC') == [b'OK', 1, 1, 1]
                assert worker.read() == [b'a', payload]
                assert admin.call('LRANGE', 'a', 0, -1) == []
                assert admin.call('LRANGE', 'b', 0, -1) == [b'preserved']
                assert primary.call('LRANGE', 'a', 0, -1) == [payload]
                admin.call('CONFIG', 'SET', 'replica-read-only', 'yes')
            finally:
                primary.close()
                admin.close()
                worker.close()


def command_statistics(port):
    observations = []
    admin = Client(port)
    try:
        for mode in ['timeout', 'unblock', 'error', 'kill', 'eof', 'acl']:
            worker = Client(port)
            try:
                identity = worker.call('CLIENT', 'ID')
                if mode == 'acl':
                    admin.call('ACL', 'SETUSER', 'stat-user', 'on',
                               '>pw', '~*', '+@all')
                    worker.call('AUTH', 'stat-user', 'pw')
                admin.call('CONFIG', 'RESETSTAT')
                worker.socket.sendall(Client.encode(
                    ['BLPOP', 'stats-key', '.1' if mode == 'timeout' else 0]))
                wait_count(admin, 1)
                if mode == 'timeout':
                    worker.read()
                elif mode == 'kill':
                    admin.call('CLIENT', 'KILL', 'ID', identity)
                elif mode == 'eof':
                    worker.close()
                elif mode == 'acl':
                    admin.call('ACL', 'SETUSER', 'stat-user', '-blpop')
                    admin.call('RPUSH', 'stats-key', 'v')
                    worker.read()
                else:
                    admin.call('CLIENT', 'UNBLOCK', identity,
                               'ERROR' if mode == 'error' else 'TIMEOUT')
                    worker.read()
                wait_count(admin, 0)
                # Cancellation may unlink before the command actor returns.
                time.sleep(.02)
                info = admin.call('INFO', 'commandstats')
                fields = {}
                for line in info.splitlines():
                    if line.startswith(b'cmdstat_blpop:'):
                        fields = dict(field.split(b'=') for field in
                                      line.split(b':', 1)[1].split(b','))
                observations.append(tuple(int(fields.get(name, 0)) for name
                    in [b'calls', b'rejected_calls', b'failed_calls']))
            finally:
                if mode != 'eof':
                    worker.close()
        assert observations == [(1, 0, 0), (1, 0, 0), (1, 0, 1),
                                (0, 0, 0), (0, 0, 0), (0, 1, 0)]
    finally:
        admin.close()
    return observations


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--pkv', required=True)
    parser.add_argument('--redis', required=True)
    args = parser.parse_args()
    backend = os.environ.get('PKV_TEST_BACKEND', 'mdb')
    with contextlib.ExitStack() as stack:
        pair = [stack.enter_context(server(binary, redis=reference,
                backend=backend)) for binary, reference in
                [(args.pkv, False), (args.redis, True)]]
        clients = [Client(port) for port in pair]
        try:
            checks = compare(clients)
            typed_checks = typed(pair)
            for function in [asynchronous, ordering, permissions,
                             watches_and_databases, command_statistics]:
                values = [function(port) for port in pair]
                assert values[0] == values[1], (function.__name__, values)
        finally:
            for client in clients:
                client.close()
    values = [cluster(binary, reference) for binary, reference in
              [(args.pkv, False), (args.redis, True)]]
    assert values[0] == values[1], values
    values = [policy(binary, reference) for binary, reference in
              [(args.pkv, False), (args.redis, True)]]
    assert values[0] == values[1], ('replica policy', values)
    persistence(args.pkv, False, backend)
    replication(args.pkv, backend)
    print('Blocking list stable differential assertions:', checks)
    print('Blocking list typed differential assertions:', typed_checks)
    print('Blocking command statistics differential cases: 6')
    print('Blocking lists, FIFO overlaps, input, ACL and cancellation passed')


if __name__ == '__main__':
    main()
