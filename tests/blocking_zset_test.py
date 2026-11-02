#!/usr/bin/env python3
"""Blocking native ZSets: parser, typed replies and asynchronous behavior."""
import argparse
import contextlib
import os
import tempfile
import time
from integration_test import Client, ErrorReply, ports, server
from resp3_client import RespClient, canonical
from blocking_list_test import wait_count


def commands(timeout):
    return [('BZPOPMIN', 'a', 'b', timeout),
            ('BZPOPMAX', 'a', 'b', timeout),
            ('BZMPOP', timeout, 2, 'a', 'b', 'MIN', 'COUNT', 2)]


def compare(clients):
    checks = 0

    def call(*args):
        nonlocal checks
        replies = [c.call(*args) for c in clients]
        assert type(replies[0]) is type(replies[1]), (args, replies)
        assert replies[0] == replies[1], (args, replies)
        checks += 1
        return replies[0]

    for timeout in ['', 'nan', '+inf', '-inf', '1e309', '-1', '-0',
                    '-0.00001', '-0.001', '0', '.0001', '1e-310',
                    '1e-9999', '0x1p0', ' 1', '1 ', b'1\0tail',
                    '9223372036854775807', '9223372036854.776']:
        for args in commands(timeout):
            call('MULTI')
            call(*args)
            call('EXEC')
    malformed = [('BZPOPMIN',), ('BZPOPMAX', 'a'), ('BZMPOP',)]
    for number in ['0', '-1', '01', '+1', '1.5', 'nan',
                   '9223372036854775807', '9223372036854775808']:
        malformed.append(('BZMPOP', 'bad', number, 'a', 'MIN'))
    for direction in ['wrong', b'MIN\0tail', 'max']:
        malformed.append(('BZMPOP', 0, 1, 'a', direction))
    for count in ['0', '-1', '01', 'nan', '9223372036854775807']:
        malformed.append(('BZMPOP', 0, 1, 'a', 'MAX', 'COUNT', count))
    malformed += [('BZMPOP', 0, 1, 'a', 'MIN', 'COUNT'),
                  ('BZMPOP', 0, 1, 'a', 'MIN', 'COUNT', 1, 'COUNT', 2),
                  ('BZMPOP', 0, 1, 'a', 'MIN', b'COUNT\0x', 2)]
    for args in malformed:
        call('MULTI')
        call(*args)
        call('EXEC')
    for direction in ['MIN', 'MAX']:
        for args in [commands(0)[0], commands(0)[1],
                     ('BZMPOP', 0, 3, 'absent', 'b', 'a', direction,
                      'COUNT', 9223372036854775807)]:
            call('DEL', 'a', 'b', 'absent')
            call('ZADD', 'b', '-inf', b'low\0', 1.5, b'z' * 40000,
                 1.5, 'equal', '+inf', 'high')
            call('MULTI')
            call(*args)
            call('EXEC')
            call('ZRANGE', 'b', 0, -1, 'WITHSCORES')
    call('DEL', 'a', 'b')
    for args in commands(0):
        call('COMMAND', 'GETKEYS', *args)
        call('COMMAND', 'GETKEYSANDFLAGS', *args)
        call('COMMAND', 'INFO', args[0])
    call('SET', 'a', 'wrong')
    for args in commands('.001'):
        call(*args)
    call('DEL', 'a', 'b')
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
                for admin in admins:
                    admin.call('DEL', 'a', 'b')
                    admin.call('ZADD', 'b', '-inf', 'low', 1.5, 'value')
                replies = [client.call(*args) for client in clients]
                assert canonical(replies[0]) == canonical(replies[1]), replies
                checked += 1
            for args in commands(0):
                for admin, client in zip(admins, clients):
                    admin.call('DEL', 'a', 'b')
                    client.socket.sendall(Client.encode(args))
                    wait_count(admin, 1)
                    admin.call('ZADD', 'a', 2.25, b'wake\0')
                replies = [client.read() for client in clients]
                assert canonical(replies[0]) == canonical(replies[1]), replies
                checked += 1
    finally:
        for client in clients + admins:
            client.close()
    return checked


def cancellation(port):
    admin = Client(port)
    observations = []
    try:
        for command in ['BLPOP', 'BZPOPMIN']:
            first, second = Client(port), Client(port)
            try:
                admin.call('DEL', 'cancel-type')
                ids = [c.call('CLIENT', 'ID') for c in [first, second]]
                first.socket.sendall(Client.encode([command, 'cancel-type', 0]))
                wait_count(admin, 1)
                second.socket.sendall(Client.encode(
                    [command, 'cancel-type', 0]))
                wait_count(admin, 2)
                admin.call('SET', 'cancel-type', 'wrong')
                assert admin.call('CLIENT', 'UNBLOCK', ids[0]) == 1
                assert first.read() is None
                time.sleep(.02)
                observations.append(admin.call('CLIENT', 'UNBLOCK', ids[1]))
                observations.append(second.read())
            finally:
                first.close()
                second.close()
        assert observations == [1, None, 1, None]
    finally:
        admin.close()
    return observations


def asynchronous(port):
    admin, worker = Client(port), Client(port)
    observations = []
    try:
        identity = worker.call('CLIENT', 'ID')
        for args in commands('.005'):
            observations.append(worker.call(*args))
        for reason in ['TIMEOUT', 'ERROR']:
            worker.socket.sendall(Client.encode(commands(0)[0]))
            wait_count(admin, 1)
            observations.append(admin.call('CLIENT', 'UNBLOCK', identity,
                                           reason))
            observations.append(worker.read())
        for args in commands(0):
            admin.call('DEL', 'a', 'b')
            worker.socket.sendall(Client.encode(['PING']) +
                                  Client.encode(args) +
                                  Client.encode(['SET', 'after', 'done']))
            assert worker.read() == b'PONG'
            wait_count(admin, 1)
            assert admin.call('GET', 'after') is None
            admin.call('ZADD', 'a', 3.5, b'x' * 40000)
            observations.append(worker.read())
            assert worker.read() == b'OK'
            assert admin.call('GET', 'after') == b'done'
            admin.call('DEL', 'after')
        worker.socket.sendall(Client.encode(['BZPOPMIN', 'kill-me', 0]))
        wait_count(admin, 1)
        assert admin.call('CLIENT', 'KILL', 'ID', identity) == 1
        wait_count(admin, 0)
    finally:
        admin.close()
        worker.close()
    return observations


def ordering(port):
    admin = Client(port)
    workers = [Client(port) for _ in range(16)]
    observations = []
    try:
        admin.call('DEL', 'fifo')
        for index, worker in enumerate(workers):
            worker.socket.sendall(Client.encode(['BZPOPMIN', 'fifo', 0]))
            wait_count(admin, index + 1)
        admin.call('ZADD', 'fifo', *[x for i in range(16)
                   for x in [i, 'member-%02d' % i]])
        for index, worker in enumerate(workers):
            assert worker.read() == [b'fifo', ('member-%02d' % index).encode(),
                                     str(index).encode()]
        for mode in range(3):
            admin.call('DEL', 'a', 'b')
            workers[0].socket.sendall(Client.encode(['BZPOPMIN', 'a', 'b', 0]))
            wait_count(admin, 1)
            admin.call('MULTI')
            if mode == 0:
                admin.call('SET', 'a', 'wrong')
            else:
                admin.call('ZADD', 'a', 1, 'old')
            admin.call('ZADD', 'b', 2, 'b')
            admin.call('DEL', 'a')
            if mode != 2:
                admin.call('ZADD', 'a', 3, 'new')
            admin.call('EXEC')
            observations.append(workers[0].read())
        admin.call('DEL', 'a', 'b')
        workers[0].socket.sendall(Client.encode(['BZPOPMIN', 'a', 'b', 0]))
        wait_count(admin, 1)
        admin.call('MULTI')
        admin.call('SET', 'a', 'wrong')
        admin.call('ZADD', 'b', 1, 'retained')
        admin.call('EXEC')
        reply = workers[0].read()
        assert isinstance(reply, ErrorReply) and reply.startswith(b'WRONGTYPE')
        observations.append(reply)
        observations.append(admin.call('ZRANGE', 'b', 0, -1))
        for _ in range(8):
            admin.call('DEL', 'a', 'b')
            older, overlap = workers[:2]
            older.socket.sendall(Client.encode(['BZPOPMIN', 'a', '.1']))
            wait_count(admin, 1)
            overlap.socket.sendall(Client.encode(['BZPOPMIN', 'a', 'b', 0]))
            wait_count(admin, 2)
            admin.call('MULTI')
            admin.call('ZADD', 'b', 1, 'b')
            admin.call('ZADD', 'a', 1, 'a')
            admin.call('EXEC')
            observations.append(overlap.read())
            observations.append(older.read())
            observations.append(admin.call('ZRANGE', 'b', 0, -1))
        return observations
    finally:
        admin.close()
        for worker in workers:
            worker.close()


def mixed_types(port):
    admin, lists, sorted_sets = Client(port), Client(port), Client(port)
    try:
        admin.call('DEL', 'mixed')
        lists.socket.sendall(Client.encode(['BLPOP', 'mixed', 0]))
        sorted_sets.socket.sendall(Client.encode(['BZPOPMIN', 'mixed', 0]))
        wait_count(admin, 2)
        admin.call('ZADD', 'mixed', 1, 'z')
        assert sorted_sets.read() == [b'mixed', b'z', b'1']
        wait_count(admin, 1)
        admin.call('RPUSH', 'mixed', 'l')
        assert lists.read() == [b'mixed', b'l']
        wait_count(admin, 0)
    finally:
        for client in [admin, lists, sorted_sets]:
            client.close()
    return True


def mixed_ordering(port):
    admin, first, second = Client(port), Client(port), Client(port)
    observations = []
    try:
        for mirror in [False, True]:
            one, two = ('BZPOPMIN', 'BLPOP') if mirror else (
                'BLPOP', 'BZPOPMIN')
            produce_a = ('ZADD', 'a', 1, 'a') if mirror else (
                'RPUSH', 'a', 'a')
            produce_b = ('RPUSH', 'b', 'b') if mirror else (
                'ZADD', 'b', 1, 'b')
            for reverse in [False, True]:
                for _ in range(8):
                    admin.call('DEL', 'a', 'b')
                    first.socket.sendall(Client.encode([one, 'a', 0]))
                    wait_count(admin, 1)
                    second.socket.sendall(Client.encode([two, 'a', 'b', 0]))
                    wait_count(admin, 2)
                    admin.call('MULTI')
                    for args in ([produce_b, produce_a] if reverse else
                                 [produce_a, produce_b]):
                        admin.call(*args)
                    admin.call('EXEC')
                    a, b = first.read(), second.read()
                    assert a == ([b'a', b'a', b'1'] if mirror else
                                 [b'a', b'a'])
                    if reverse:
                        assert isinstance(b, ErrorReply) and b.startswith(
                            b'WRONGTYPE'), b
                    else:
                        assert b == ([b'b', b'b'] if mirror else
                                     [b'b', b'b', b'1'])
                    observations.append((a, b))
        return observations
    finally:
        for client in [admin, first, second]:
            client.close()


def mixed_temporary_types(port):
    admin, first, second, unrelated = [Client(port) for _ in range(4)]
    observations = []
    try:
        unrelated.call('SELECT', 3)
        identity = unrelated.call('CLIENT', 'ID')
        for phase in range(5):
            globally_blocked = phase > 0
            cancelled_inside = phase in (2, 4)
            admin.call('DEL', 'a', 'b')
            if globally_blocked:
                unrelated.socket.sendall(Client.encode(
                    ['BLPOP', 'other-db', 0]))
                wait_count(admin, 1)
            first.socket.sendall(Client.encode(['BZPOPMIN', 'a', '.1']))
            wait_count(admin, 1 + int(globally_blocked))
            second.socket.sendall(Client.encode(['BZPOPMIN', 'a', 'b', 0]))
            wait_count(admin, 2 + int(globally_blocked))
            admin.call('MULTI')
            if phase >= 3:
                admin.call('CONFIG', 'SET', 'replica-read-only', 'no')
            admin.call('RPUSH', 'a', 'temporary-list')
            if cancelled_inside:
                admin.call('CLIENT', 'UNBLOCK', identity)
            admin.call('ZADD', 'b', 1, 'b')
            admin.call('DEL', 'a')
            admin.call('ZADD', 'a', 1, 'a')
            admin.call('EXEC')
            a, b = first.read(), second.read()
            assert a == ([b'a', b'a', b'1'] if globally_blocked else None)
            assert b == ([b'b', b'b', b'1'] if globally_blocked else
                         [b'a', b'a', b'1'])
            observations.append((a, b))
            if globally_blocked:
                if not cancelled_inside:
                    assert admin.call('CLIENT', 'UNBLOCK', identity) == 1
                assert unrelated.read() is None
            wait_count(admin, 0)
        return observations
    finally:
        for client in [admin, first, second, unrelated]:
            client.close()


def producers(port):
    admin, worker = Client(port), Client(port)
    observations = []
    cases = [('ZINCRBY', 'target', 1, 'one'),
             ('ZUNIONSTORE', 'target', 1, 'source'),
             ('ZINTERSTORE', 'target', 1, 'source'),
             ('ZDIFFSTORE', 'target', 1, 'source'),
             ('ZRANGESTORE', 'target', 'source', 0, -1),
             ('COPY', 'source', 'target'),
             ('RENAME', 'source', 'target'),
             ('RENAMENX', 'source', 'target'),
             ('GEOADD', 'target', 0, 0, 'm'),
             ('GEOSEARCHSTORE', 'target', 'geo-source', 'FROMMEMBER', 'm',
              'BYRADIUS', 1, 'm', 'STOREDIST'),
             ('GEORADIUS', 'geo-source', 0, 0, 1, 'm', 'STORE', 'target'),
             ('GEORADIUSBYMEMBER', 'geo-source', 'm', 1, 'm',
              'STOREDIST', 'target')]
    try:
        for args in cases:
            admin.call('DEL', 'target', 'source', 'geo-source')
            admin.call('ZADD', 'source', 1, 'one', 2, 'two')
            admin.call('GEOADD', 'geo-source', 0, 0, 'm')
            worker.socket.sendall(Client.encode(['BZPOPMIN', 'target', 0]))
            wait_count(admin, 1)
            admin.call(*args)
            observations.append(worker.read())
        worker.call('SELECT', 3)
        admin.call('SELECT', 3)
        admin.call('DEL', 'target')
        admin.call('SELECT', 0)
        admin.call('DEL', 'target')
        admin.call('ZADD', 'target', 1, 'db-transfer')
        worker.socket.sendall(Client.encode(['BZPOPMIN', 'target', 0]))
        wait_count(admin, 1)
        assert admin.call('COPY', 'target', 'target', 'DB', 3) == 1
        observations.append(worker.read())
        worker.call('SELECT', 0)
        return observations
    finally:
        admin.close()
        worker.close()


def permissions(port):
    admin, worker, watcher = Client(port), Client(port), Client(port)
    try:
        admin.call('ACL', 'SETUSER', 'zblocked', 'on', '>pw', '~allowed*',
                   '+bzpopmin')
        assert worker.call('AUTH', 'zblocked', 'pw') == b'OK'
        denied = worker.call('BZPOPMIN', 'denied', 0)
        assert isinstance(denied, ErrorReply) and denied.startswith(b'NOPERM')
        worker.socket.sendall(Client.encode(['BZPOPMIN', 'allowed-key', 0]))
        wait_count(admin, 1)
        admin.call('ACL', 'SETUSER', 'zblocked', '-bzpopmin')
        admin.call('ZADD', 'allowed-key', 1, 'preserved')
        denied = worker.read()
        assert isinstance(denied, ErrorReply) and denied.startswith(b'NOPERM')
        assert admin.call('ZRANGE', 'allowed-key', 0, -1) == [b'preserved']
        worker.call('AUTH', 'default', '')
        watcher.call('WATCH', 'watched-z')
        watcher.call('MULTI')
        watcher.call('PING')
        worker.socket.sendall(Client.encode(['BZPOPMIN', 'watched-z', 0]))
        wait_count(admin, 1)
        admin.call('ZADD', 'watched-z', 1, 'v')
        assert worker.read() == [b'watched-z', b'v', b'1']
        assert watcher.call('EXEC') is None
        watcher.call('WATCH', 'miss-z')
        assert worker.call('BZPOPMIN', 'miss-z', '.001') is None
        watcher.call('MULTI')
        watcher.call('PING')
        assert watcher.call('EXEC') == [b'PONG']
        worker.call('SELECT', 3)
        admin.call('SELECT', 3)
        worker.socket.sendall(Client.encode(['BZMPOP', 0, 1, 'db3', 'MAX']))
        wait_count(admin, 1)
        admin.call('ZADD', 'db3', 2.5, 'v')
        assert worker.read() == [b'db3', [[b'v', b'2.5']]]
    finally:
        for client in [admin, worker, watcher]:
            client.close()
    return True


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
            worker.socket.sendall(Client.encode(['BZPOPMIN', 'policy', 0]))
            wait_count(admin, 1)
            admin.call('MULTI')
            admin.call('CONFIG', 'SET', 'replica-read-only', 'yes')
            admin.call('ZADD', 'policy', 1, 'preserved')
            committed = admin.call('EXEC')
            return committed, worker.read(), admin.call('ZRANGE', 'policy',
                                                       0, -1)
        finally:
            admin.close()
            worker.close()


def cluster(binary, reference):
    with server(binary, redis=reference, cluster=True) as port:
        client = Client(port)
        try:
            return [client.call(*args) for args in
                    [('BZPOPMIN', '{a}x', '{b}x', 0),
                     ('BZPOPMAX', '{a}x', 0),
                     ('BZMPOP', 0, 2, '{a}x', '{b}x', 'MIN'),
                     ('BZPOPMIN', '{a}x', '{b}x', 'bad'),
                     ('BZMPOP', 'bad', 0, '{a}x', 'MIN')]]
        finally:
            client.close()


def persistence(binary, backend):
    if backend not in ('rdb', 'wdb'):
        return
    with tempfile.TemporaryDirectory(prefix='pkv-blocking-zset-') as storage:
        for stage in range(2):
            with server(binary, backend=backend, storage_path=storage) as port:
                admin, worker = Client(port), Client(port)
                try:
                    admin.call('SELECT', 3)
                    worker.call('SELECT', 3)
                    if stage == 0:
                        worker.socket.sendall(Client.encode(
                            ['BZPOPMIN', 'disk', 0]))
                        wait_count(admin, 1)
                        admin.call('ZADD', 'disk', 1, b'p' * 40000, 2, 'left')
                        assert worker.read() == [b'disk', b'p' * 40000, b'1']
                    assert admin.call('ZRANGE', 'disk', 0, -1) == [b'left']
                finally:
                    admin.close()
                    worker.close()


def replication(binary, backend):
    with server(binary, backend=backend) as source:
        upstream = '127.0.0.1:%d' % (source + 10000)
        with server(binary, backend=backend,
                    replica_rpc_source=upstream) as dest:
            primary, admin, worker = Client(source), Client(dest), Client(dest)
            try:
                deadline = time.monotonic() + 8
                while True:
                    primary.call('SET', 'link-ready', 'yes')
                    if admin.call('GET', 'link-ready') == b'yes':
                        break
                    assert time.monotonic() < deadline
                    time.sleep(.02)
                admin.call('CONFIG', 'SET', 'replica-read-only', 'no')
                worker.socket.sendall(Client.encode(
                    ['BZPOPMIN', 'a', 'b', 0]))
                wait_count(admin, 1)
                primary.call('MULTI')
                primary.call('SET', 'a', 'wrong')
                primary.call('ZADD', 'b', 1, 'preserved')
                primary.call('DEL', 'a')
                primary.call('ZADD', 'a', 2, b'r' * 40000)
                assert primary.call('EXEC') == [b'OK', 1, 1, 1]
                assert worker.read() == [b'a', b'r' * 40000, b'2']
                assert admin.call('ZRANGE', 'a', 0, -1) == []
                assert admin.call('ZRANGE', 'b', 0, -1) == [b'preserved']
                assert primary.call('ZRANGE', 'a', 0, -1) == [b'r' * 40000]
            finally:
                for client in [primary, admin, worker]:
                    client.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--pkv', required=True)
    parser.add_argument('--redis', required=True)
    args = parser.parse_args()
    backend = os.environ.get('PKV_TEST_BACKEND', 'mdb')
    with contextlib.ExitStack() as stack:
        endpoints = [stack.enter_context(server(binary, redis=reference,
                     backend=backend)) for binary, reference in
                     [(args.pkv, False), (args.redis, True)]]
        clients = [Client(port) for port in endpoints]
        try:
            checks = compare(clients)
            typed_checks = typed(endpoints)
            for function in [cancellation, asynchronous, ordering,
                             mixed_types, mixed_ordering, mixed_temporary_types,
                             producers, permissions]:
                replies = [function(port) for port in endpoints]
                assert replies[0] == replies[1], (function.__name__, replies)
        finally:
            for client in clients:
                client.close()
    for function in [policy, cluster]:
        replies = [function(binary, reference) for binary, reference in
                   [(args.pkv, False), (args.redis, True)]]
        assert replies[0] == replies[1], (function.__name__, replies)
    persistence(args.pkv, backend)
    replication(args.pkv, backend)
    print('Blocking ZSet stable differential assertions:', checks)
    print('Blocking ZSet typed differential assertions:', typed_checks)
    print('Blocking ZSets, mixed types, cancellation and replay passed')
    print('Cross-type EXEC order and global blocked-type checks passed')


if __name__ == '__main__':
    main()
