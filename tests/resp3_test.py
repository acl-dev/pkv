#!/usr/bin/env python3
"""Typed command replies and protocol changes in pipelines and EXEC."""
import argparse
from pathlib import Path
import subprocess
import time
from contextlib import ExitStack

from integration_test import server
from resp3_client import RespClient, Reply, canonical, mapping


def hello(client, *args):
    reply = client.call('HELLO', *args)
    info = mapping(reply)
    assert set(info) == {b'server', b'version', b'proto', b'id', b'mode',
                         b'role', b'modules'}
    assert info[b'proto'].kind == b':' and info[b'id'].value > 0
    for key in [b'server', b'version', b'mode', b'role']:
        assert info[key].kind == b'$'
    assert info[b'modules'].kind == b'*', info
    if info[b'server'].value == b'pkv':
        assert info[b'modules'].value == []
    assert reply.kind == (b'%' if info[b'proto'].value == 3 else b'*')
    return info


def exercise(port, reference):
    a, b = RespClient(port), RespClient(reference)
    checks = 0

    def compare(*args):
        nonlocal checks
        x, y = a.call(*args), b.call(*args)
        assert canonical(x) == canonical(y), (args, x, y)
        checks += 1
        return x

    try:
        for c in [a, b]:
            identity = hello(c)[b'id']
            assert hello(c, 3)[b'id'] == identity
            assert c.call('CLIENT', 'ID') == identity
        assert hello(a)[b'server'].value == b'pkv'
        for command in [('PING',), ('ECHO', b'x\0\xff'), ('GET', 'missing'),
                        ('MGET', 'missing', 'absent'), ('LPOP', 'missing'),
                        ('LPOP', 'missing', 2), ('LMPOP', 1, 'missing', 'LEFT'),
                        ('ZMPOP', 1, 'missing', 'MIN'),
                        ('SET', 's', 'abc'), ('INCRBYFLOAT', 'float', '1.5'),
                        ('HINCRBYFLOAT', 'hf', 'field', '2.5'),
                        ('HGETALL', 'none'), ('SMEMBERS', 'none'),
                        ('SPOP', 'none', 1), ('SRANDMEMBER', 'none', 1),
                        ('HSET', 'hash', 'a', 'one', 'b', b'v\0\xff'),
                        ('HGETALL', 'hash'), ('HMGET', 'hash', 'a', 'none'),
                        ('HGETDEL', 'hash', 'FIELDS', 2, 'a', 'a'),
                        ('HGETALL', 'hash'),
                        ('HRANDFIELD', 'hash', -2, 'WITHVALUES'),
                        ('SADD', 'set', 'a', 'b'), ('SMEMBERS', 'set'),
                        ('SINTER', 'set', 'none'), ('SUNION', 'set', 'none'),
                        ('SDIFF', 'set', 'none'),
                        ('SADD', 'single', 'x'), ('SPOP', 'single', 1),
                        ('SADD', 'single', 'x'), ('SRANDMEMBER', 'single', -2),
                        ('SET', 'lcs1', 'ohmytext'),
                        ('SET', 'lcs2', 'mynewtext'),
                        ('LCS', 'lcs1', 'lcs2', 'IDX', 'WITHMATCHLEN'),
                        ('PFADD', 'hll-typed', 'a', 'b'),
                        ('PFCOUNT', 'hll-typed'),
                        ('PFCOUNT', 'hll-typed', 'absent'),
                        ('PFMERGE', 'hll-out', 'hll-typed'),
                        ('PFCOUNT', 'hll-out'), ('GET', 'hll-out'),
                        ('PFADD', 'hll-typed', 'a'),
                        ('SET', 'hll-bad', 'oops'),
                        ('PFCOUNT', 'hll-bad'),
                        ('GEOADD', 'geo-typed', 13.361389, 38.115556, 'a',
                         15.087269, 37.502669, 'b'),
                        ('GEOPOS', 'geo-typed', 'a', 'missing'),
                        ('GEOHASH', 'geo-typed', 'a', 'missing'),
                        ('GEODIST', 'geo-typed', 'a', 'b', 'km'),
                        ('GEODIST', 'geo-typed', 'a', 'missing'),
                        ('GEOPOS', 'geo-absent', 'missing'),
                        ('GEOPOS', 'geo-typed'), ('GEOHASH', 'geo-typed'),
                        ('GEOPOS', 's', 'a'),
                        ('GEOSEARCH', 'geo-typed', 'FROMMEMBER', 'a',
                         'BYRADIUS', 1000, 'km', 'WITHDIST', 'WITHHASH',
                         'WITHCOORD', 'ASC'),
                        ('GEOSEARCH', 'geo-typed', 'FROMMEMBER', 'a',
                         'BYBOX', 0, 0, 'm', 'COUNT', 1, 'ANY'),
                        ('GEOSEARCHSTORE', 'geo-found', 'geo-typed',
                         'FROMMEMBER', 'a', 'BYRADIUS', 1000, 'km',
                         'STOREDIST'),
                        ('ZRANGE', 'geo-found', 0, -1, 'WITHSCORES'),
                        ('GEOSEARCH', 'geo-absent', 'FROMMEMBER', 'missing',
                         'BYRADIUS', 1, 'm'),
                        ('GEOSEARCH', 's', 'FROMMEMBER', 'missing',
                         'BYRADIUS', 1, 'm'),
                        ('GEORADIUS_RO', 'geo-typed', 13, 38, 1000, 'km',
                         'WITHDIST', 'WITHHASH', 'WITHCOORD', 'ASC'),
                        ('GEORADIUSBYMEMBER_RO', 'geo-typed', 'a', 0, 'm'),
                        ('GEORADIUSBYMEMBER', 'geo-typed', 'a', 0, 'm',
                         'STOREDIST', 'radius-typed'),
                        ('ZRANGE', 'radius-typed', 0, -1, 'WITHSCORES'),
                        ('GEORADIUSBYMEMBER_RO', 'absent-geo', 'missing',
                         'invalid', 'invalid')]:
            compare(*command)
        for scores in [('1.5', 'a', '-2.75', 'b', 'inf', 'c', '-inf', 'd'),
                       ('0', 'a', '0', 'b', '1', 'c', '1', 'd')]:
            compare('DEL', 'z')
            compare('ZADD', 'z', *scores)
            for command in [('ZSCORE', 'z', 'a'),
                            ('ZMSCORE', 'z', 'a', 'missing', 'b'),
                            ('ZRANK', 'z', 'a', 'WITHSCORE'),
                            ('ZREVRANK', 'z', 'a', 'WITHSCORE'),
                            ('ZRANGE', 'z', 0, -1, 'WITHSCORES'),
                            ('ZREVRANGE', 'z', 0, -1, 'WITHSCORES'),
                            ('ZRANGEBYSCORE', 'z', '-inf', '+inf',
                             'WITHSCORES'),
                            ('ZUNION', 1, 'z', 'WITHSCORES'),
                            ('ZINTER', 1, 'z', 'WITHSCORES'),
                            ('ZDIFF', 1, 'z', 'WITHSCORES')]:
                compare(*command)
            compare('ZPOPMIN', 'z')
            compare('ZPOPMAX', 'z', 1)
            compare('ZMPOP', 1, 'z', 'MIN', 'COUNT', 2)
            compare('ZPOPMIN', 'z')
            compare('ZPOPMAX', 'z', 1)
        compare('ZADD', 'one-z', 1.25, 'x')
        compare('ZRANDMEMBER', 'one-z', -2, 'WITHSCORES')
        compare('ZADD', 'one-z', 'INCR', 1.5, 'x')
        compare('ZINCRBY', 'one-z', -2.75, 'x')
        compare('ZADD', 'pop-one-z', '-inf', b'a\0' * 256, '+inf', b'b\xff')
        compare('ZPOPMAX', 'pop-one-z', 1)
        compare('ZPOPMIN', 'pop-one-z')
        compare('ZPOPMIN', 'pop-one-z', 1)
        pairs = [part for i in range(80) for part in (i / 2, b'm\0' + bytes([i]))]
        members = [b'm\0' + bytes([i]) for i in range(40)]
        for extra in ([], [b'fragment' * 12]):
            compare('SADD', 'pop-bulk-set', *(members + extra))
            compare('SPOP', 'pop-bulk-set', 64)
            compare('EXISTS', 'pop-bulk-set')
        compare('ZADD', 'pop-bulk-z', *pairs)
        score_members = [b'm\0' + bytes([i]) for i in range(79, -1, -1)]
        compare('ZMSCORE', 'pop-bulk-z', *(score_members + ['missing'] +
                 score_members))
        compare('ZRANGE', 'pop-bulk-z', 0, -1, 'WITHSCORES')
        compare('ZRANGE', 'pop-bulk-z', 8, 71, 'WITHSCORES')
        compare('ZREVRANGE', 'pop-bulk-z', 8, 71, 'WITHSCORES')
        compare('ZRANGE', 'pop-bulk-z', 35.5, 4, 'BYSCORE', 'REV', 'WITHSCORES')
        compare('ZREVRANGE', 'pop-bulk-z', 0, -1, 'WITHSCORES')
        compare('ZPOPMAX', 'pop-bulk-z', 80)
        compare('EXISTS', 'pop-bulk-z')
        excluded = {b'client', b'command', b'config', b'cluster',
                    b'cluster|syncslots', b'cluster|addslave'}
        names = a.call('COMMAND', 'LIST').value
        for name in [n.value for n in names] + [b'nonexistent']:
            if name in (b'acl', b'config', b'pubsub'):
                x = a.call('COMMAND', 'INFO', name).value[0]
                y = b.call('COMMAND', 'INFO', name).value[0]
                assert x.kind == y.kind == b'*' and len(x.value) == 10
                assert canonical(Reply(x.kind, x.value[:9])) == \
                    canonical(Reply(y.kind, y.value[:9]))
                children = (
                    b'cat deluser dryrun genpass getuser help list load log '
                    b'save setuser users whoami') if name == b'acl' else (
                    b'get set help resetstat') if name == b'config' else (
                    b'channels numpat numsub shardchannels shardnumsub help')
                expected = {name + b'|' + child for child in children.split()}
                actual = {row.value[0].value: canonical(row)
                          for row in x.value[9].value}
                reference_rows = {row.value[0].value: canonical(row)
                                  for row in y.value[9].value}
                assert set(actual) == expected
                assert actual == {key: reference_rows[key] for key in expected}
                checks += 1
            elif name not in excluded:
                compare('COMMAND', 'INFO', name)
        compare('COMMAND', 'GETKEYSANDFLAGS', 'SET', 's', 'value')
        docs = a.call('COMMAND', 'DOCS', 'get', 'hello')
        assert docs.kind == b'%'
        assert all(v.kind == b'%' for v in mapping(docs).values())
        assert a.call('CONFIG', 'GET', 'appendonly').kind == b'%'
        for c in [a, b]:
            assert c.call('HSCAN', 'hash', 0).value[1].kind == b'*'
            values = c.call('ZSCAN', 'one-z', 0).value[1]
            assert values.kind == b'*' and all(v.kind == b'$' for v in
                                              values.value)
        return checks
    finally:
        a.close()
        b.close()


def negotiation(port, reference):
    a, b = RespClient(port), RespClient(reference)
    try:
        for c in [a, b]:
            identity = hello(c)[b'id']
            for version in [2, 3, 2, 3]:
                assert hello(c, version, 'SETNAME', 'negotiation')[b'id'] == \
                    identity
                assert c.call('CLIENT', 'GETNAME') == Reply(b'$',
                                                         b'negotiation')
            info = hello(c, 3, 'AUTH', 'default', b'password\0bytes')
            assert info[b'proto'] == Reply(b':', 3)
        for args in [(1,), (4,), ('bad',), ('02',),
                     (b'3\0',), ('9223372036854775808',),
                     (2, 'SETNAME'), (2, 'AUTH'), (2, 'AUTH', 'default'),
                     (2, 'AUTH', 'missing', 'pass'),
                     (2, 'SETNAME', 'bad name'), (2, 'wrong'),
                     (2, b'bad\r\n+FAKE'),
                     (2, 'SETNAME', 'new', 'wrong'),
                     (2, 'SETNAME', 'bad name', 'SETNAME', 'valid')]:
            x, y = a.call('HELLO', *args), b.call('HELLO', *args)
            assert x == y and x.kind == b'-', (args, x, y)
            for c in [a, b]:
                assert hello(c)[b'proto'] == Reply(b':', 3)
                assert c.call('CLIENT', 'GETNAME').value == b'negotiation'
                assert c.call('GET', 'absent') == Reply(b'_', None)
        for c in [a, b]:
            assert hello(c, 2, 'SETNAME', 'one', 'SETNAME', '')[b'proto'] == \
                Reply(b':', 2)
            assert c.call('CLIENT', 'GETNAME') == Reply(b'$', None)
            hello(c, 3)
            assert c.call('RESET') == Reply(b'+', b'RESET')
            assert c.call('GET', 'absent') == Reply(b'$', None)
            assert hello(c)[b'proto'] == Reply(b':', 2)
        for token in [b'SETNAME\0x', b'AUTH\0x']:
            assert a.call('HELLO', 3, token, 'default', 'pass').kind == b'-'
            assert hello(a)[b'proto'] == Reply(b':', 2)
    finally:
        a.close()
        b.close()


def watched(port, reference):
    for selected in [port, reference]:
        a, b = RespClient(selected), RespClient(selected)
        try:
            hello(a, 3)
            assert a.call('SET', 'watch-proto', 'old') == Reply(b'+', b'OK')
            assert a.call('WATCH', 'watch-proto') == Reply(b'+', b'OK')
            assert b.call('SET', 'watch-proto', 'new') == Reply(b'+', b'OK')
            assert a.call('MULTI') == Reply(b'+', b'OK')
            assert a.call('GET', 'watch-proto') == Reply(b'+', b'QUEUED')
            assert a.call('EXEC') == Reply(b'_', None)
            assert a.call('PING') == Reply(b'+', b'PONG')
        finally:
            a.close()
            b.close()


def pipelines(port, reference):
    for selected in [port, reference]:
        c = RespClient(selected)
        try:
            commands = [('GET', 'absent'), ('HELLO', 3), ('GET', 'absent'),
                        ('HELLO', 2), ('GET', 'absent'), ('HELLO', 3),
                        ('RESET',), ('GET', 'absent')]
            c.socket.sendall(b''.join(c.encode(args) for args in commands))
            replies = [c.read() for _ in commands]
            assert [r.kind for r in replies] == \
                [b'$', b'%', b'_', b'*', b'$', b'%', b'+', b'$']
            request = [('MULTI',), ('GET', 'absent'), ('HELLO', 3),
                       ('GET', 'absent'), ('HGETALL', 'hash'),
                       ('HELLO', 2), ('GET', 'absent'), ('EXEC',),
                       ('GET', 'absent')]
            c.socket.sendall(b''.join(c.encode(args) for args in request))
            replies = [c.read() for _ in request]
            result = replies[-2]
            assert result.kind == b'*'
            assert [r.kind for r in result.value] == \
                [b'$', b'%', b'_', b'%', b'*', b'$']
            assert replies[-1].kind == b'$'
            assert c.call('HSET', 'hmget-wire', b'f\0', b'v\0\xff', 'empty',
                          '') == Reply(b':', 2)
            fields = (b'f\0', 'missing', 'empty', b'f\0', b'f\0') * 3
            get = ('HMGET', 'hmget-wire', *fields)
            missing = ('HMGET', 'absent', *fields)
            commands = [get, ('HELLO', 3), get, missing, ('MULTI',),
                        get, ('EXEC',), ('HELLO', 2), get]
            c.socket.sendall(b''.join(c.encode(args) for args in commands))
            replies = [c.read() for _ in commands]
            for index, null_kind in [(0, b'$'), (2, b'_'), (8, b'$')]:
                expected = Reply(b'*', [
                    Reply(null_kind, None) if field == 'missing' else
                    Reply(b'$', b'' if field == 'empty' else b'v\0\xff')
                    for field in fields])
                assert replies[index] == expected
            assert replies[3] == Reply(b'*', [Reply(b'_', None)] * len(fields))
            assert replies[6] == Reply(b'*', [replies[2]])
            assert c.call('ZADD', 'zmscore-wire', '1.5', b'm\0',
                          '-inf', 'low') == Reply(b':', 2)
            members = (b'm\0', 'missing', 'low') * 44
            scores = ('ZMSCORE', 'zmscore-wire', *members)
            commands = [scores, ('HELLO', 3), scores, ('MULTI',),
                        scores, ('EXEC',), ('HELLO', 2), scores]
            c.socket.sendall(b''.join(c.encode(args) for args in commands))
            replies = [c.read() for _ in commands]
            resp2 = Reply(b'*', [Reply(b'$', b'1.5'), Reply(b'$', None),
                                 Reply(b'$', b'-inf')] * 44)
            resp3 = Reply(b'*', [Reply(b',', 1.5), Reply(b'_', None),
                                 Reply(b',', float('-inf'))] * 44)
            assert replies[0] == replies[7] == resp2
            assert replies[2] == resp3
            assert replies[5] == Reply(b'*', [resp3])
            values = [bytes([i, 0, 255]) for i in range(47)]
            for protocol in (2, 3):
                hello(c, protocol)
                key = 'list-pop-wire'
                assert c.call('RPUSH', key, *values) == Reply(b':', 47)
                assert c.call('RPOP', key, 0) == Reply(b'*', [])
                commands = [('LPOP', key, 15), ('MULTI',),
                            ('RPOP', key, 16), ('LPOP', key, 32), ('EXEC',),
                            ('RPOP', key, 32)]
                c.socket.sendall(b''.join(c.encode(args) for args in commands))
                replies = [c.read() for _ in commands]
                assert replies[0] == Reply(b'*', [Reply(b'$', v) for v in values[:15]])
                assert replies[4] == Reply(b'*', [
                    Reply(b'*', [Reply(b'$', v) for v in reversed(values[31:])]),
                    Reply(b'*', [Reply(b'$', v) for v in values[15:31]])])
                assert replies[5] == Reply(b'*' if protocol == 2 else b'_', None)
                assert c.call('RPUSH', key, values[0]) == Reply(b':', 1)
                assert c.call('RPOP', key, 1) == Reply(b'*', [Reply(b'$', values[0])])
                assert c.call('RPOP', key, 1) == Reply(
                    b'*' if protocol == 2 else b'_', None)
                assert c.call('RPOP', key) == Reply(
                    b'$' if protocol == 2 else b'_', None)
            assert c.call('ZADD', 'zrange-wire',
                          *(part for i in range(128)
                            for part in (i, f'member:{i}'))) == Reply(b':', 128)
            forward = ('ZRANGE', 'zrange-wire', 0, 127, 'WITHSCORES')
            reverse = ('ZREVRANGE', 'zrange-wire', 0, 127, 'withscores')
            for protocol in (2, 3):
                hello(c, protocol)
                assert c.call('ZADD', 'zrange-wire', 1, 'member:1') == Reply(b':', 0)
                before = c.call(*forward)
                before_reverse = c.call(*reverse)
                commands = [forward, forward, reverse, reverse,
                            ('ZADD', 'zrange-wire', 1.5, 'member:1'),
                            forward, forward]
                c.socket.sendall(b''.join(c.encode(args) for args in commands))
                replies = [c.read() for _ in commands]
                assert replies[0] == replies[1] == before
                assert replies[2] == replies[3] == before_reverse
                assert replies[4] == Reply(b':', 0)
                assert replies[5] == replies[6] == c.call(*forward)
                assert replies[5] != before
            assert c.call('SET', 'hmget-wire', 'string') == Reply(b'+', b'OK')
            assert c.call(*get).kind == b'-'
            pairs = [(f'f:{i}', b'value') for i in range(126)]
            pairs.extend([(b'f\0', b'v\0\xff' * 4000), ('empty', b'')])
            assert c.call('HSET', 'hmget-dense',
                          *(part for pair in pairs for part in pair)) == Reply(b':', 128)
            hello(c, 3)
            query = pairs[::-1] + [('missing', None), pairs[-2]]
            reply = c.call('HMGET', 'hmget-dense', *(name for name, _ in query))
            assert reply == Reply(b'*', [
                Reply(b'_', None) if value is None else Reply(b'$', value)
                for _, value in query])
            large = b'x' * (300 * 1024)
            assert c.call('HSET', 'hmget-dense', 'empty', large) == Reply(b':', 0)
            reply = c.call('HMGET', 'hmget-dense', *(name for name, _ in query))
            assert reply == Reply(b'*', [
                Reply(b'_', None) if value is None else
                Reply(b'$', large if name == 'empty' else value)
                for name, value in query])
        finally:
            c.close()

def identical_hash_write(port, reference):
    for selected in (port, reference):
        reader, writer = RespClient(selected), RespClient(selected)
        try:
            key, field, value = b'hash-watch\0', b'f\0', b'v\0'
            deadline = 4102444800123
            assert reader.call('HSET', key, field, value) == Reply(b':', 1)
            assert reader.call('PEXPIREAT', key, deadline) == Reply(b':', 1)
            assert reader.call('WATCH', key) == Reply(b'+', b'OK')
            writer.socket.sendall(writer.encode(('HSET', key, field, value)) * 16)
            assert [writer.read() for _ in range(16)] == [Reply(b':', 0)] * 16
            assert reader.call('MULTI') == Reply(b'+', b'OK')
            assert reader.call('HGET', key, field) == Reply(b'+', b'QUEUED')
            assert reader.call('EXEC') == Reply(b'*', None)
            assert reader.call('HGET', key, field) == Reply(b'$', value)
            assert reader.call('PEXPIRETIME', key) == Reply(b':', deadline)
            pair = b'hash-pair\0'
            assert reader.call('HSET', pair, field, b'first', field, value) == Reply(b':', 1)
            assert reader.call('HGET', pair, field) == Reply(b'$', value)
            assert reader.call('HMSET', pair, field, value, field, b'last') == Reply(b'+', b'OK')
            assert reader.call('HGET', pair, field) == Reply(b'$', b'last')
            assert reader.call('HSET', pair, b'z\0', b'z', field, value) == Reply(b':', 1)
            assert reader.call('HLEN', pair) == Reply(b':', 2)
            commands = [('HGET', pair, field),
                        ('HSET', pair, b'z\0', b'updated', field, b'new'),
                        ('HGET', pair, field),
                        ('HSET', pair, field, b'ignored', field, value),
                        ('HGET', pair, field)]
            writer.socket.sendall(b''.join(writer.encode(args) for args in commands))
            assert [writer.read() for _ in commands] == [
                Reply(b'$', value), Reply(b':', 0), Reply(b'$', b'new'),
                Reply(b':', 0), Reply(b'$', value)]
        finally:
            reader.close()
            writer.close()


def string_pipeline(port, protocol):
    key = b'string-pipeline\0'
    with ExitStack() as clients:
        reader, writer = RespClient(port), RespClient(port)
        clients.callback(reader.close)
        clients.callback(writer.close)
        for c in (reader, writer):
            assert c.call('HELLO', protocol).kind == (b'%' if protocol == 3 else b'*')
        ok = Reply(b'+', b'OK')
        missing = Reply(b'_', None) if protocol == 3 else Reply(b'$', None)
        nil_exec = Reply(b'_', None) if protocol == 3 else Reply(b'*', None)

        def batch(commands):
            writer.socket.sendall(b''.join(writer.encode(args) for args in commands))
            return [writer.read() for _ in commands]

        commands = [('GET', key)] * 2
        expected = [missing] * 2
        for i in range(38):
            value = b'\0\xff' + bytes([i])
            commands.extend([('SET', key, value), ('gEt', key)])
            expected.extend([ok, Reply(b'$', value)])
        assert batch(commands) == expected

        large = b'L' * 8193
        assert reader.call('SET', key, large) == ok
        assert reader.call('PEXPIREAT', key, 4102444800123) == Reply(b':', 1)
        assert batch([('GET', key)] * 5 + [('SET', key, b'a'), ('GET', key)]) == (
            [Reply(b'$', large)] * 5 + [ok, Reply(b'$', b'a')])
        assert reader.call('PTTL', key) == Reply(b':', -1)
        assert reader.call('SET', key, 'expired', 'PX', 30) == ok
        time.sleep(0.05)
        assert batch([('GET', key)] * 2 + [('SET', key, b'a'), ('GET', key)]) == [
            missing, missing, ok, Reply(b'$', b'a')]
        assert reader.call('PTTL', key) == Reply(b':', -1)
        assert reader.call('DEL', key) == Reply(b':', 1)
        assert reader.call('HSET', key, 'field', 'value') == Reply(b':', 1)
        replies = batch([('GET', key)] * 2 + [('SET', key, b'a'), ('GET', key)])
        assert all(r.kind == b'-' and r.value.startswith(b'WRONGTYPE') for r in replies[:2])
        assert replies[2:] == [ok, Reply(b'$', b'a')]

        def watch_exec():
            assert reader.call('MULTI') == ok
            assert reader.call('GET', key) == Reply(b'+', b'QUEUED')
            return reader.call('EXEC')

        assert reader.call('WATCH', key) == ok
        assert batch([('GET', key)] * 64) == [Reply(b'$', b'a')] * 64
        assert watch_exec() == Reply(b'*', [Reply(b'$', b'a')])
        assert reader.call('WATCH', key) == ok
        assert batch([('SET', key, b'a'), ('GET', key)] * 32) == [ok, Reply(b'$', b'a')] * 32
        assert watch_exec() == nil_exec
        assert reader.call('DEL', key) == Reply(b':', 1)


def topology(binary):
    with server(binary, cluster=True) as port:
        c = RespClient(port)
        try:
            info = hello(c, 3, 'SETNAME', '{unserved}name')
            assert info[b'mode'].value == b'cluster'
            assert info[b'role'].value == b'master'
            assert c.call('MULTI') == Reply(b'+', b'OK')
            assert c.call('HELLO', 2) == Reply(b'+', b'QUEUED')
            assert c.call('EXEC').value[0].kind == b'*'
        finally:
            c.close()
    with server(binary) as source:
        rpc = '127.0.0.1:%d' % (source + 10000)
        with server(binary, replica_rpc_source=rpc) as receiver:
            c = RespClient(receiver)
            try:
                assert hello(c, 3)[b'role'].value == b'replica'
            finally:
                c.close()


def cli(binary, port, identity):
    executable = Path(binary).with_name('redis-cli')
    assert executable.is_file(), executable
    result = subprocess.run([str(executable), '-3', '-h', '127.0.0.1',
                             '-p', str(port), '--raw'],
                            input=b'HSET cli-h f value\nHGETALL cli-h\n'
                            b'HELLO 2\nGET absent\n', capture_output=True,
                            check=True, timeout=10)
    assert result.stdout.startswith(b'1\nf value\n'), result
    assert identity in result.stdout, result
    print('redis-cli RESP3 negotiation passed')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--pkv', required=True)
    parser.add_argument('--redis', required=True)
    args = parser.parse_args()
    with server(args.redis, redis=True) as ref, server(args.pkv) as port:
        checks = exercise(port, ref)
        negotiation(port, ref)
        pipelines(port, ref)
        identical_hash_write(port, ref)
        for selected in (port, ref):
            for protocol in (2, 3):
                string_pipeline(selected, protocol)
        watched(port, ref)
        cli(args.redis, ref, b'redis')
        cli(args.redis, port, b'pkv')
    topology(args.pkv)
    print('RESP3 typed differential assertions:', checks)


if __name__ == '__main__':
    main()
