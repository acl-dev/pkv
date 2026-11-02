#!/usr/bin/env python3
"""Command metadata and key discovery without target execution."""
import argparse
from integration_test import Client, ErrorReply, server


def mapping(items):
    return dict(zip(items[::2], items[1::2]))


def basic(port):
    client = Client(port)
    try:
        rows = client.call('COMMAND')
        assert rows == client.call('COMMAND', 'INFO')
        roots = {row[0]: row for row in rows}
        assert len(rows) == len(roots) == client.call('COMMAND', 'COUNT')
        assert all(len(row) == 10 for row in rows)
        listed = set(client.call('COMMAND', 'LIST'))
        expected = set(roots)
        for row in rows:
            expected.update(sub[0] for sub in row[9])
        assert listed == expected
        assert b'command|getkeysandflags' in listed
        assert b'cluster|syncslots' in listed
        assert {b'config|get', b'config|set', b'config|help',
                b'config|resetstat'} <= set(listed)
        missing = client.call('COMMAND', 'INFO', 'missing', b'GET\0alias')
        assert missing == [None, None]
        assert client.call('COMMAND', 'INFO', 'GeT')[0] == roots[b'get']
        assert client.call('COMMAND', 'DOCS', 'missing') == []
        assert client.call('COMMAND', 'LIST', 'FILTERBY', 'MODULE',
                           'missing') == []
        for category in ('read', 'write', 'string', 'fast', 'nosuchcategory'):
            found = set(client.call('COMMAND', 'LIST', 'FILTERBY', 'ACLCAT',
                                    category.upper()))
            all_rows = list(rows) + [sub for row in rows for sub in row[9]]
            assert found == {row[0] for row in all_rows
                             if b'@' + category.encode() in row[6]}
        for pattern, expected_names in [('GET*', {n for n in listed
                                                 if n.startswith(b'get')}),
                                        ('command|*', {n for n in listed
                                                      if n.startswith(
                                                          b'command|')})]:
            assert set(client.call('COMMAND', 'LIST', 'FILTERBY', 'PATTERN',
                                   pattern)) == expected_names
        docs = mapping(client.call('COMMAND', 'DOCS', 'GET', 'missing'))
        assert list(docs) == [b'get']
        assert mapping(docs[b'get'])[b'group'] == b'string'
        assert isinstance(client.call('COMMAND', 'HELP'), list)
        for args in [('COUNT', 'extra'), ('HELP', 'extra'), ('GETKEYS',),
                     ('GETKEYSANDFLAGS',), ('LIST', 'x'),
                     ('LIST', 'FILTERBY', 'bad', 'x'), (b'INFO\0x',)]:
            assert isinstance(client.call('COMMAND', *args), ErrorReply), args
        return roots
    finally:
        client.close()


def discovery(port, reference, roots):
    client, other = Client(port), Client(reference)
    count = 0
    try:
        cases = [
            ('MSET', b'a\0b', 'v', 'second', 'v'),
            ('MSET', 'a', 'v', 'orphan'),
            ('COPY', 'a', 'a', 'DB', '1', 'REPLACE'),
            ('SET', 'a', 'v', 'GET', 'PX', '100'),
            ('SET', 'a', 'v', 'NX'),
            ('BITFIELD', 'a'),
            ('BITFIELD', 'a', 'GET', 'i8', '0'),
            ('BITFIELD', 'a', 'OVERFLOW', 'FAIL', 'GET', 'u8', '0'),
            ('BITFIELD', 'a', 'SET', 'u8', '0', '1'),
            ('BITFIELD', 'a', 'invalid'),
            ('ZUNIONSTORE', 'dst', '2', 'a', 'b', 'WEIGHTS', '1', '2'),
            ('ZUNIONSTORE', 'dst', '0'),
            ('ZMPOP', '2', 'a', 'b', 'MIN', 'COUNT', '3'),
            ('LMPOP', '2', 'a', 'b', 'LEFT'),
            ('SINTERCARD', '2', 'a', 'b', 'LIMIT', '0'),
            ('SINTERCARD', '9223372036854775807', 'a'),
            ('GET',), ('GET', 'a', 'extra'), ('PING',),
            ('CONFIG', 'GET', '*'), ('CONFIG', 'missing'),
            ('COMMAND', 'INFO', 'SET'), ('config|get', '*'), ('missing',),
        ]
        for name, row in roots.items():
            if name in (b'command', b'config', b'cluster'):
                continue
            expected = other.call('COMMAND', 'INFO', name)[0]
            assert row[:9] == expected[:9], name
            if not row[8]:
                continue
            args = [name] + [b'key'] * (abs(row[1]) - 1)
            for spec in row[8]:
                spec = mapping(spec)
                begin = mapping(spec[b'begin_search'])
                find = mapping(spec[b'find_keys'])
                if find[b'type'] == b'keynum':
                    index = mapping(begin[b'spec'])[b'index']
                    index += mapping(find[b'spec'])[b'keynumidx']
                    args[index] = b'1'
            cases.append(tuple(args))
        for target in cases:
            for operation in ('GETKEYS', 'GETKEYSANDFLAGS'):
                got = client.call('COMMAND', operation, *target)
                expected = other.call('COMMAND', operation, *target)
                assert type(got) is type(expected) and got == expected, (
                    operation, target, got, expected)
                count += 1
        assert client.call('COMMAND', 'GETKEYS', b'GET\0x', 'a') == \
            ErrorReply(b'ERR Invalid command specified')
        return count
    finally:
        client.close()
        other.close()


def isolation(port):
    a, b = Client(port), Client(port)
    try:
        assert a.call('SET', 'command-marker', 'old') == b'OK'
        assert a.call('COMMAND', 'GETKEYS', 'SET', 'command-marker', 'new') == \
            [b'command-marker']
        assert a.call('GET', 'command-marker') == b'old'
        assert isinstance(a.call('COMMAND', 'GETKEYS', 'SELECT', '1'),
                          ErrorReply)
        assert a.call('GET', 'command-marker') == b'old'
        assert a.call('COMMAND', 'GETKEYS', 'WATCH', 'unwatched') == \
            [b'unwatched']
        assert b.call('SET', 'unwatched', 'x') == b'OK'
        assert a.call('MULTI') == b'OK'
        for args in [('SELECT', 1), ('COMMAND', 'GETKEYS', 'MGET', 'a', 'b'),
                     ('SELECT', 0), ('GET', 'command-marker')]:
            assert a.call(*args) == b'QUEUED'
        assert a.call('EXEC') == [b'OK', [b'a', b'b'], b'OK', b'old']
    finally:
        a.close()
        b.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--pkv', required=True)
    parser.add_argument('--redis', required=True)
    args = parser.parse_args()
    with server(args.redis, redis=True) as ref, server(args.pkv) as port:
        roots = basic(port)
        count = discovery(port, ref, roots)
        isolation(port)
    with server(args.pkv, cluster=True) as port:
        c = Client(port)
        try:
            assert c.call('COMMAND', 'GETKEYS', 'MGET', '{a}x', '{b}x') == \
                [b'{a}x', b'{b}x']
            assert c.call('MULTI') == b'OK'
            assert c.call('COMMAND', 'GETKEYS', 'MGET', '{a}x', '{b}x') == \
                b'QUEUED'
            assert c.call('EXEC') == [[b'{a}x', b'{b}x']]
        finally:
            c.close()
    print('Command metadata comparisons:', count)


if __name__ == '__main__':
    main()
