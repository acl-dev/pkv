#!/usr/bin/env python3
"""ACL LOG differential semantics, protocol types, limits and failure paths."""
import argparse
from concurrent.futures import ThreadPoolExecutor

from integration_test import Client, ErrorReply, server
from resp3_client import RespClient

FIELDS = {b'count', b'reason', b'context', b'object', b'username',
          b'age-seconds', b'client-info', b'entry-id', b'timestamp-created',
          b'timestamp-last-updated'}


def rows(client, count=None):
    raw = client.call('ACL', 'LOG', *([] if count is None else [count]))
    assert isinstance(raw, list), raw
    result = []
    for row in raw:
        assert len(row) == 20
        entry = dict(zip(row[::2], row[1::2]))
        assert set(entry) == FIELDS
        assert entry[b'count'] > 0 and entry[b'entry-id'] >= 0
        assert entry[b'timestamp-created'] > 0
        assert entry[b'timestamp-last-updated'] >= entry[b'timestamp-created']
        assert float(entry[b'age-seconds']) >= 0
        info = dict(part.split(b'=', 1)
                    for part in entry[b'client-info'].split())
        assert int(info[b'id']) > 0 and b'cmd' in info and b'user' in info
        assert b'super-private-password' not in entry[b'client-info']
        result.append(entry)
    return result


def stable(entries):
    return [tuple(row[key] for key in [b'count', b'reason', b'context',
            b'object', b'username', b'entry-id']) for row in entries]


def observe(binary, reference):
    observations = []
    with server(binary, redis=reference) as port:
        admin, client = Client(port), Client(port)
        try:
            assert admin.call('ACL', 'SETUSER', 'alice', 'on', '>secret',
                              '+get', '+multi', '+exec', '+discard',
                              '+client|id', '+auth', '+hello',
                              '~allow:*') == b'OK'
            assert admin.call('ACL', 'LOG', 'RESET') == b'OK'

            def capture(count=None):
                entries = rows(admin, count)
                observations.append(stable(entries))
                return entries

            assert capture() == []
            assert isinstance(client.call('AUTH', 'unused'), ErrorReply)
            assert rows(admin) == []
            for command in [('ACL', 'LOG', 'bad'),
                            ('ACL', 'LOG', '9223372036854775808'),
                            ('ACL', 'LOG', '1', 'extra')]:
                observations.append(admin.call(*command))
                assert isinstance(observations[-1], ErrorReply)
            assert capture(-1) == [] and capture(0) == []
            assert isinstance(client.call('aUtH', 'ghost',
                                         'super-private-password'), ErrorReply)
            entries = capture()
            assert entries[0][b'object'] == b'aUtH'
            assert isinstance(client.call('HELLO', 3, 'AUTH', 'ghost',
                                         'super-private-password'), ErrorReply)
            capture()
            assert client.call('AUTH', 'alice', 'secret') == b'OK'
            assert isinstance(client.call('SET', 'allow:key', 'hidden-value'),
                              ErrorReply)
            capture()
            key = b'denied:\0key'
            for _ in range(3):
                assert isinstance(client.call('GET', key), ErrorReply)
            entries = capture()
            assert entries[0][b'count'] == 3 and entries[0][b'object'] == key
            original = entries[0]
            assert isinstance(client.call('GET', 'other'), ErrorReply)
            assert isinstance(client.call('GET', key), ErrorReply)
            entries = capture()
            assert entries[0][b'count'] == 4
            assert entries[0][b'entry-id'] == original[b'entry-id']
            assert entries[0][b'timestamp-created'] == \
                original[b'timestamp-created']
            assert isinstance(client.call('CLIENT', 'LIST'), ErrorReply)
            entries = capture()
            assert entries[0][b'object'] == b'client|list'
            before = stable(rows(admin, 100))
            admin.call('ACL', 'DRYRUN', 'alice', 'GET', key)
            assert stable(rows(admin, 100)) == before
            assert client.call('MULTI') == b'OK'
            assert isinstance(client.call('SET', 'allow:key', 'hidden-value'),
                              ErrorReply)
            assert client.call('DISCARD') == b'OK'
            entries = capture()
            assert entries[0][b'context'] == b'multi'
            assert client.call('MULTI') == b'OK'
            assert client.call('GET', 'allow:key') == b'QUEUED'
            assert admin.call('ACL', 'SETUSER', 'alice', '-get') == b'OK'
            result = client.call('EXEC')
            assert len(result) == 1 and isinstance(result[0], ErrorReply)
            entries = capture()
            assert entries[0][b'object'] == b'get'
            assert entries[0][b'context'] == b'multi'
            latest_id = max(row[b'entry-id'] for row in entries)
            assert len(capture(1)) == 1
            typed = RespClient(port)
            try:
                assert typed.call('HELLO', 3).kind == b'%'
                reply = typed.call('ACL', 'LOG')
                assert reply.kind == b'*' and reply.value
                for row in reply.value:
                    assert row.kind == b'%' and len(row.value) == 20
                    entry = {row.value[i].value: row.value[i + 1]
                             for i in range(0, len(row.value), 2)}
                    assert entry[b'age-seconds'].kind == b','
                    assert entry[b'entry-id'].kind == b':'
                    assert entry[b'client-info'].kind == b'$'
            finally:
                typed.close()
            assert admin.call('ACL', 'LOG', 'rEsEt') == b'OK'
            assert capture() == []
            assert isinstance(client.call('GET', 'allow:key'), ErrorReply)
            entries = capture()
            assert entries[0][b'entry-id'] == latest_id + 1
            assert admin.call('ACL', 'LOG', b'ReSeT\0ignored') == b'OK'
            assert capture() == []
        finally:
            admin.close()
            client.close()
    return observations


def limits(binary, reference):
    for maximum in (None, 0, 2):
        with server(binary, redis=reference, acllog_max_len=maximum) as port:
            admin, client = Client(port), Client(port)
            try:
                assert admin.call('ACL', 'SETUSER', 'a', 'on', 'nopass',
                                  '+get') == b'OK'
                assert client.call('AUTH', 'a', 'unused') == b'OK'
                total = 142 if maximum is None else 12
                for i in range(total):
                    assert isinstance(client.call('GET', str(i)), ErrorReply)
                entries = rows(admin, 1000)
                assert len(entries) == (128 if maximum is None else maximum)
                if maximum is None:
                    assert len(rows(admin)) == 10
                    assert entries[0][b'object'] == b'141'
                    assert entries[-1][b'object'] == b'14'
                elif maximum:
                    assert [row[b'object'] for row in entries] == [b'11', b'10']
            finally:
                admin.close()
                client.close()


def unauthenticated(binary, reference):
    with server(binary, redis=reference, password='root') as port:
        admin, client = Client(port), Client(port)
        try:
            assert admin.call('AUTH', 'root') == b'OK'
            assert admin.call('ACL', 'LOG', 'RESET') == b'OK'
            for command in [('PING',), ('AUTH',),
                            ('HELLO', 3, 'AUTH', 'default')]:
                assert isinstance(client.call(*command), ErrorReply)
            assert rows(admin, 100) == []
            assert isinstance(client.call('AUTH', 'bad'), ErrorReply)
            assert rows(admin)[0][b'username'] == b'default'
        finally:
            admin.close()
            client.close()


def invalid_limit(binary, reference):
    rejected = False
    try:
        with server(binary, redis=reference, acllog_max_len=-1):
            raise AssertionError('negative ACL log limit started serving')
    except AssertionError as error:
        assert 'server exited with' in str(error), error
        rejected = True
    assert rejected


def concurrent(binary):
    with server(binary) as port:
        admin = Client(port)
        try:
            assert admin.call('ACL', 'SETUSER', 'a', 'on', 'nopass',
                              '+get') == b'OK'
            assert admin.call('ACL', 'LOG', 'RESET') == b'OK'

            def worker(_):
                client = Client(port)
                try:
                    assert client.call('AUTH', 'a', 'unused') == b'OK'
                    for _ in range(100):
                        reply = client.call('GET', 'same')
                        assert isinstance(reply, ErrorReply)
                finally:
                    client.close()
            with ThreadPoolExecutor(max_workers=8) as pool:
                list(pool.map(worker, range(8)))
            entries = rows(admin, 100)
            assert len(entries) == 1 and entries[0][b'count'] == 800
        finally:
            admin.close()


def self_revocation(binary):
    with server(binary) as port:
        admin, client = Client(port), Client(port)
        try:
            assert admin.call('ACL', 'SETUSER', 'self', 'on', 'nopass',
                              '+@all', '~*', '&*') == b'OK'
            assert admin.call('ACL', 'LOG', 'RESET') == b'OK'
            assert client.call('AUTH', 'self', 'unused') == b'OK'
            assert client.call('MULTI') == b'OK'
            assert client.call('ACL', 'DELUSER', 'self') == b'QUEUED'
            assert client.call('SET', 'after-self', 'never') == b'QUEUED'
            result = client.call('EXEC')
            assert result[0] == 1 and isinstance(result[1], ErrorReply)
            assert client.socket.recv(1) == b''
            assert admin.call('GET', 'after-self') is None
            entries = rows(admin, 100)
            assert len(entries) == 1
            assert entries[0][b'reason'] == b'command'
            assert entries[0][b'context'] == b'multi'
            assert entries[0][b'object'] == b'set'
            assert entries[0][b'username'] == b'self'
        finally:
            admin.close()
            client.close()
    print('ACL LOG self-revocation command diagnostics passed')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--pkv', required=True)
    parser.add_argument('--redis', required=True)
    args = parser.parse_args()
    actual = observe(args.pkv, False)
    expected = observe(args.redis, True)
    assert actual == expected, (actual, expected)
    limits(args.pkv, False)
    limits(args.redis, True)
    unauthenticated(args.pkv, False)
    unauthenticated(args.redis, True)
    invalid_limit(args.pkv, False)
    invalid_limit(args.redis, True)
    concurrent(args.pkv)
    self_revocation(args.pkv)
    print('ACL LOG common observations:', len(actual))
    print('ACL LOG protocols, binary objects, grouping, '
          'bounds and races passed')


if __name__ == '__main__':
    main()
