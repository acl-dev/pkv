#!/usr/bin/env python3
"""MULTI isolation, owned queues, runtime errors and optimistic WATCH."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import os
import threading
import time

from integration_test import Client, ErrorReply, server


def protocol(port):
    a, b = Client(port), Client(port)
    count = 0

    def check(command, expected, client=a):
        nonlocal count
        actual = client.call(*command)
        assert actual == expected, (command, actual, expected)
        count += 1

    def error(command, prefix, client=a):
        nonlocal count
        actual = client.call(*command)
        assert isinstance(actual, ErrorReply) and actual.startswith(prefix), (
            command, actual, prefix)
        count += 1

    try:
        error(('EXEC',), b'ERR EXEC without MULTI')
        error(('DISCARD',), b'ERR DISCARD without MULTI')
        check(('UNWATCH',), b'OK')
        check(('MULTI',), b'OK')
        error(('MULTI',), b'ERR MULTI calls can not be nested')
        error(('WATCH', 'tx:key'), b'ERR WATCH inside MULTI')
        check(('SET', 'tx:key', b'owned\0\r\n' * 9000), b'QUEUED')
        check(('GET', 'tx:key'), None, b)
        check(('GET', 'tx:key'), b'QUEUED')
        check(('MGET', 'tx:key', 'tx:missing'), b'QUEUED')
        value = b'owned\0\r\n' * 9000
        check(('EXEC',), [b'OK', value, [value, None]])
        check(('MULTI',), b'OK')
        check(('SET', 'tx:discard', 'x'), b'QUEUED')
        check(('DISCARD',), b'OK')
        check(('GET', 'tx:discard'), None)
        check(('MULTI',), b'OK')
        check(('SET', 'tx:abort', 'x'), b'QUEUED')
        error(('NO-SUCH-COMMAND',), b'ERR unknown command')
        check(('SET', 'tx:abort', 'later'), b'QUEUED')
        error(('EXEC',), b'EXECABORT')
        check(('GET', 'tx:abort'), None)
        check(('MULTI',), b'OK')
        error(('SET', 'key'), b'ERR wrong number of arguments')
        error(('EXEC',), b'EXECABORT')
        check(('MULTI',), b'OK')
        check(('SET', 'tx:wrongtype', 'x'), b'QUEUED')
        check(('LPUSH', 'tx:wrongtype', 'a'), b'QUEUED')
        check(('SET', 'tx:after-error', 'yes'), b'QUEUED')
        replies = a.call('EXEC')
        assert replies[0] == b'OK' and replies[2] == b'OK'
        assert isinstance(replies[1], ErrorReply)
        count += 1
        check(('GET', 'tx:after-error'), b'yes')
        check(('MULTI',), b'OK')
        check(('EXEC',), [])
        # SELECT is evaluated in queue order and persists after EXEC.
        check(('MULTI',), b'OK')
        check(('SELECT', 1), b'QUEUED')
        check(('SET', 'tx:db', 'one'), b'QUEUED')
        check(('SELECT', 0), b'QUEUED')
        check(('GET', 'tx:db'), b'QUEUED')
        check(('EXEC',), [b'OK', b'OK', b'OK', None])
        check(('SELECT', 1), b'OK')
        check(('GET', 'tx:db'), b'one')
        check(('SELECT', 0), b'OK')
        # Parser roots are reused within a pipeline, and all replies survive.
        commands = [('MULTI',), ('SET', 'tx:pipe', 'p'),
                    ('GET', 'tx:pipe'), ('EXEC',), ('GET', 'tx:pipe')]
        a.socket.sendall(b''.join(Client.encode(c) for c in commands))
        assert [a.read() for _ in commands] == [
            b'OK', b'QUEUED', b'QUEUED', [b'OK', b'p'], b'p']
        count += 1
        # Repeated WATCH never refreshes a previously invalidated snapshot.
        check(('WATCH', 'tx:pipe', 'tx:pipe'), b'OK')
        check(('SET', 'tx:pipe', 'p'), b'OK', b)
        check(('WATCH', 'tx:pipe'), b'OK')
        check(('MULTI',), b'OK')
        check(('SET', 'tx:pipe', 'lost'), b'QUEUED')
        check(('EXEC',), None)
        check(('GET', 'tx:pipe'), b'p')
        check(('MULTI',), b'OK')
        check(('EXEC',), [])
        check(('WATCH', 'tx:pipe'), b'OK')
        check(('UNWATCH',), b'OK')
        check(('SET', 'tx:pipe', 'changed'), b'OK', b)
        check(('MULTI',), b'OK')
        check(('UNWATCH',), b'QUEUED')
        check(('EXEC',), [b'OK'])
        # Missing deletion, failed NX and plain reads do not dirty WATCH.
        for command in [('DEL', 'tx:absent'),
                        ('GET', 'tx:pipe'),
                        ('SET', 'tx:pipe', 'x', 'NX'),
                        ('SADD', 'tx:set', 'member'),
                        ('SREM', 'tx:set', 'absent')]:
            if command[0] == 'SADD':
                check(command, 1, b)
                continue
            key = command[1]
            check(('WATCH', key), b'OK')
            b.call(*command)
            check(('MULTI',), b'OK')
            check(('EXEC',), [])
        # All watched DBs remain active across SELECT; different DB writes
        # to an equal key do not conflict.
        check(('WATCH', 'tx:db-watch'), b'OK')
        check(('SELECT', 1), b'OK')
        check(('WATCH', 'tx:db-watch'), b'OK')
        check(('SET', 'tx:db-watch', 'zero'), b'OK', b)
        check(('MULTI',), b'OK')
        check(('EXEC',), None)
        check(('SELECT', 0), b'OK')
        check(('WATCH', 'tx:db-watch'), b'OK')
        check(('SELECT', 1), b'OK', b)
        check(('SET', 'tx:db-watch', 'one'), b'OK', b)
        check(('MULTI',), b'OK')
        check(('EXEC',), [])
        check(('SELECT', 0), b'OK', b)
        # WATCH of an expired key is a clean missing-key snapshot. Earlier
        # watchers still abort, even if no command actively expires the key.
        check(('SET', 'tx:expiry', 'x', 'PX', 80), b'OK', b)
        check(('WATCH', 'tx:expiry'), b'OK')
        time.sleep(.12)
        check(('WATCH', 'tx:expiry'), b'OK', b)
        check(('GET', 'tx:expiry'), None, b)
        check(('MULTI',), b'OK')
        check(('EXEC',), None)
        check(('MULTI',), b'OK', b)
        check(('EXEC',), [], b)
        # DISCARD and EXECABORT release the optimistic snapshot.
        for ending in ('DISCARD', 'EXECABORT'):
            check(('WATCH', 'tx:pipe'), b'OK')
            check(('MULTI',), b'OK')
            if ending == 'DISCARD':
                check(('DISCARD',), b'OK')
            else:
                error(('GET',), b'ERR wrong number')
                error(('EXEC',), b'EXECABORT')
            check(('SET', 'tx:pipe', 'new'), b'OK', b)
            check(('MULTI',), b'OK')
            check(('EXEC',), [])
        orphan = Client(port)
        check(('MULTI',), b'OK', orphan)
        check(('SET', 'tx:orphan', 'x'), b'QUEUED', orphan)
        check(('QUIT',), b'OK', orphan)
        orphan.close()
        check(('GET', 'tx:orphan'), None)
    finally:
        a.close()
        b.close()
    return count


def strict_command_names(port):
    c = Client(port)
    try:
        reply = c.call(b'MULTI\0ignored')
        assert isinstance(reply, ErrorReply) and reply.startswith(
            b'ERR unknown command'), reply
        assert c.call('MULTI') == b'OK'
        reply = c.call(b'EXEC\0ignored')
        assert isinstance(reply, ErrorReply) and reply.startswith(
            b'ERR unknown command'), reply
        assert c.call('EXEC').startswith(b'EXECABORT')
    finally:
        c.close()
    return 4


def watch_semantics(port):
    a, b = Client(port), Client(port)
    seeds = {
        'string': ('SET', 'tx:semantic', 'x'),
        'number': ('SET', 'tx:semantic', '1'),
        'list': ('RPUSH', 'tx:semantic', 'x', 'y'),
        'set': ('SADD', 'tx:semantic', 'x'),
        'hash': ('HSET', 'tx:semantic', 'f', 'x'),
        'zset': ('ZADD', 'tx:semantic', 1, 'x'),
    }
    cases = [
        ('string', ('SET', 'x'), True),
        ('string', ('APPEND', ''), True),
        ('string', ('SETRANGE', 0, ''), False),
        ('string', ('GETEX',), False),
        ('string', ('GETEX', 'PERSIST'), False),
        ('number', ('INCRBY', 0), True),
        ('number', ('INCRBYFLOAT', '0'), True),
        ('string', ('SETBIT', 0, 0), False),
        ('string', ('BITFIELD', 'SET', 'u8', 0, 120), False),
        ('list', ('LTRIM', 0, -1), True),
        ('list', ('LSET', 0, 'x'), True),
        ('list', ('LREM', 0, 'absent'), False),
        ('list', ('LINSERT', 'BEFORE', 'bad', 'x'), False),
        ('set', ('SADD', 'x'), False),
        ('set', ('SREM', 'absent'), False),
        ('hash', ('HSET', 'f', 'x'), True),
        ('hash', ('HDEL', 'absent'), False),
        ('zset', ('ZADD', 1, 'x'), False),
        ('zset', ('ZINCRBY', 0, 'x'), False),
        ('zset', ('ZREMRANGEBYRANK', 3, 4), False),
        ('list', ('LMOVE', 'tx:semantic', 'LEFT', 'LEFT'), True),
        ('set', ('SMOVE', 'tx:semantic', 'x'), False),
        ('string', ('RENAME', 'tx:semantic'), False),
        ('string', ('COPY', 'tx:semantic', 'REPLACE'), False),
    ]
    try:
        for kind, operation, dirty in cases:
            b.call('DEL', 'tx:semantic')
            b.call(*seeds[kind])
            assert a.call('WATCH', 'tx:semantic') == b'OK'
            b.call(operation[0], 'tx:semantic', *operation[1:])
            assert a.call('MULTI') == b'OK'
            reply = a.call('EXEC')
            assert reply == (None if dirty else []), (operation, reply)
        key = b'tx:hash\0pipeline'
        deadline = 4102444800123
        assert b.call('HSET', key, b'f\0', b'abc') == 1
        assert b.call('PEXPIREAT', key, deadline) == 1
        assert a.call('WATCH', key) == b'OK'
        commands = [('HSET', key, b'f\0', b'abc'), ('HGET', key, b'f\0')] * 35
        b.socket.sendall(b''.join(Client.encode(command) for command in commands))
        assert [b.read() for _ in commands] == [0, b'abc'] * 35
        assert a.call('MULTI') == b'OK'
        assert a.call('EXEC') is None

        pairs = [part for i in range(62) for part in (f'field:{i}', b'value')]
        assert b.call('HSET', key, *pairs) == 62
        commands = [('HSET', key, b'f\0', b'new'), ('HSET', key, 'overflow', 'last'),
                    ('HGET', key, b'f\0'), ('HGET', key, 'overflow')]
        b.socket.sendall(b''.join(Client.encode(command) for command in commands))
        assert [b.read() for _ in commands] == [0, 1, b'new', b'last']
        assert b.call('HLEN', key) == 64
        assert b.call('PEXPIRETIME', key) == deadline
        grouped = b'tx:hash\0grouped'
        fields = [f'element:{i:012d}' for i in range(512)]
        assert b.call('HSET', grouped, *(p for f in fields for p in (f, 'abc'))) == len(fields)
        assert b.call('PEXPIREAT', grouped, deadline) == 1
        assert a.call('WATCH', grouped) == b'OK'
        commands = [('HSET', grouped, fields[0], 'abc'), ('HGET', grouped, fields[0])] * 35
        b.socket.sendall(b''.join(Client.encode(command) for command in commands))
        assert [b.read() for _ in commands] == [0, b'abc'] * 35
        assert a.call('MULTI') == b'OK' and a.call('EXEC') is None
        assert a.call('WATCH', grouped) == b'OK'
        assert b.call('HSETNX', grouped, fields[0], 'abc') == 0
        assert a.call('MULTI') == b'OK' and a.call('EXEC') == []
        assert b.call('PEXPIRETIME', grouped) == deadline
        # A successful explicit expiry dirties even an unchanged deadline.
        for command in ('GETEX', 'PEXPIREAT'):
            deadline = int(time.time() * 1000) + 60000
            b.call('SET', 'tx:semantic', 'x', 'PXAT', deadline)
            assert a.call('WATCH', 'tx:semantic') == b'OK'
            if command == 'GETEX':
                b.call(command, 'tx:semantic', 'PXAT', deadline)
            else:
                b.call(command, 'tx:semantic', deadline)
            assert a.call('MULTI') == b'OK'
            assert a.call('EXEC') is None, command
    finally:
        a.close()
        b.close()
    return len(cases) + 2


def cluster_routing(binary):
    with server(os.path.abspath(binary), cluster=True) as port:
        c = Client(port)
        try:
            a, b = 'tx:{one}:a', 'tx:{two}:b'
            slots = [c.call('CLUSTER', 'KEYSLOT', k) for k in (a, b)]
            assert slots[0] != slots[1]
            assert c.call('WATCH', a, b).startswith(b'CROSSSLOT')
            assert c.call('WATCH', a).startswith(b'CLUSTERDOWN')
            assert c.call('MULTI') == b'OK'
            assert c.call('MGET', a, b).startswith(b'CROSSSLOT')
            assert c.call('GET', a).startswith(b'CLUSTERDOWN')
            assert c.call('EXEC').startswith(b'EXECABORT')
            assert c.call('MULTI') == b'OK'
            assert c.call('GET', a).startswith(b'CLUSTERDOWN')
            assert c.call('EXEC').startswith(b'EXECABORT')
            assert c.call('CLUSTER', 'ADDSLOTS', *slots) == b'OK'
            assert c.call('WATCH', a) == b'OK'
            assert c.call('SET', a, 'seed') == b'OK'
            assert c.call('WATCH', a, b).startswith(b'CROSSSLOT')
            assert c.call('UNWATCH') == b'OK'
            assert c.call('DEL', a) == 1
            assert c.call('MULTI') == b'OK'
            assert c.call('SET', a, 'first') == b'QUEUED'
            assert c.call('SET', b, 'second') == b'QUEUED'
            assert c.call('EXEC').startswith(b'CROSSSLOT')
            assert c.call('GET', a) is None
            assert c.call('GET', b) is None
            # Single-slot queued commands execute, including nested arrays.
            assert c.call('MULTI') == b'OK'
            assert c.call('MSET', a, 'first', 'tx:{one}:b', 'second') == (
                b'QUEUED')
            assert c.call('MGET', a, 'tx:{one}:b') == b'QUEUED'
            assert c.call('EXEC') == [b'OK', [b'first', b'second']]
            assert c.call('MULTI') == b'OK'
            assert c.call('ZUNIONSTORE', a, 1, b).startswith(b'CROSSSLOT')
            assert c.call('EXEC').startswith(b'EXECABORT')
        finally:
            c.close()
    return 29


def concurrent(port):
    start = threading.Barrier(5)

    def increment(_):
        c = Client(port)
        retries = 0
        try:
            start.wait(timeout=5)
            for _ in range(35):
                while True:
                    assert c.call('WATCH', 'tx:counter') == b'OK'
                    value = int(c.call('GET', 'tx:counter') or b'0')
                    assert c.call('MULTI') == b'OK'
                    assert c.call('SET', 'tx:counter', value + 1) == b'QUEUED'
                    reply = c.call('EXEC')
                    if reply is not None:
                        assert reply == [b'OK']
                        break
                    retries += 1
            return retries
        finally:
            c.close()

    def coherent(_):
        c = Client(port)
        try:
            start.wait(timeout=5)
            for n in range(140):
                assert c.call('MULTI') == b'OK'
                assert c.call('SET', 'tx:left', n) == b'QUEUED'
                assert c.call('SET', 'tx:right', n) == b'QUEUED'
                assert c.call('EXEC') == [b'OK', b'OK']
        finally:
            c.close()

    def reader(_):
        c = Client(port)
        try:
            start.wait(timeout=5)
            for _ in range(280):
                values = c.call('MGET', 'tx:left', 'tx:right')
                assert values[0] == values[1], values
        finally:
            c.close()

    with ThreadPoolExecutor(max_workers=5) as pool:
        futures = [pool.submit(increment, i) for i in range(3)]
        futures += [pool.submit(coherent, 0), pool.submit(reader, 0)]
        for future in futures:
            future.result(timeout=30)
    c = Client(port)
    try:
        assert c.call('GET', 'tx:counter') == b'105'
    finally:
        c.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--pkv', required=True)
    parser.add_argument('--redis')
    args = parser.parse_args()
    for binary, redis in ((args.pkv, False), (args.redis, True)):
        if not binary:
            continue
        with server(os.path.abspath(binary), redis) as port:
            count = protocol(port) + watch_semantics(port)
            concurrent(port)
            if not redis:
                print('pkv: %d strict transaction name checks passed' %
                      strict_command_names(port))
            print('%s: %d transaction assertions and CAS/isolation passed' % (
                'Redis' if redis else 'pkv', count))
        if not redis:
            print('pkv: %d cluster transaction checks passed' %
                  cluster_routing(binary))


if __name__ == '__main__':
    main()
