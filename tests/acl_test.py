#!/usr/bin/env python3
"""Named ACL users, selectors, multi-key permissions and live revocation."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import threading

from integration_test import Client, ErrorReply, server
from resp3_client import RespClient, mapping


def pairs(items):
    return dict(zip(items[::2], items[1::2]))


def differential(port, reference):
    a, b = Client(port), Client(reference)
    x, y = Client(port), Client(reference)
    checks = 0

    def compare(admin, *args):
        nonlocal checks
        left, right = (a, b) if admin else (x, y)
        value, expected = left.call(*args), right.call(*args)
        assert value == expected, (args, value, expected)
        assert isinstance(value, ErrorReply) == isinstance(expected, ErrorReply)
        checks += 1
        return value

    try:
        compare(True, 'ACL', 'WHOAMI')
        for args in [(), ('SETUSER',), ('GETUSER',), ('DELUSER',),
                     ('DRYRUN', 'none'), ('UNKNOWN',), ('GETUSER', 'x', 'y')]:
            compare(True, 'ACL', *args)
        compare(True, 'ACL', 'SETUSER', 'bob', 'on', 'nopass', '-@all')
        compare(False, 'AUTH', 'bob', 'anything')
        for args in [('PING',), ('GET', 'key'), ('SET', 'key', 'value'),
                     ('MULTI',), ('CLIENT', 'ID'), ('ACL', 'WHOAMI')]:
            compare(False, *args)
        compare(True, 'ACL', 'SETUSER', 'bob', '+get', '+mget', '+set',
                '+copy', '+watch', '%R~source:*', '%W~target:*')
        compare(True, 'SET', 'source:a', 'v')
        for args in [('GET', 'source:a'), ('GET', 'target:a'),
                     ('SET', 'target:a', 'new'),
                     ('SET', 'target:a', 'v', 'GET'),
                     ('SET', 'target:a', 'v', 'IFEQ', 'GET'),
                     ('MGET', 'source:a', 'source:b'),
                     ('MGET', 'source:a', 'target:a'),
                     ('COPY', 'source:a', 'target:b'),
                     ('COPY', 'target:a', 'source:b'),
                     ('WATCH', 'target:a')]:
            compare(False, *args)
        compare(True, 'ACL', 'SETUSER', 'bob', 'reset', 'on', 'nopass',
                '+get', '~one:*', '(+mget', '~two:*)')
        for args in [('MGET', 'two:a', 'two:b'),
                     ('MGET', 'one:a', 'two:b'), ('GET', 'one:a'),
                     ('GET', 'two:a')]:
            compare(False, *args)
        compare(True, 'ACL', 'SETUSER', 'diagnostic', 'on', 'nopass',
                '+mget', '~one:*', '(+mget ~two:*)')
        compare(True, 'ACL', 'DRYRUN', 'diagnostic', 'MGET', 'one:a', 'two:b')
        compare(True, 'ACL', 'DRYRUN', 'diagnostic', 'MGET', 'one:a',
                b'two:\0b\r\n')
        compare(True, 'ACL', 'SETUSER', 'bob', 'reset', 'on', 'nopass',
                '+@string', '~*', '-set', '+client|id', '+select|0')
        for args in [('GET', 'key'), ('SET', 'key', 'v'), ('SELECT', 0),
                     ('SELECT', 1), ('CLIENT', 'GETNAME')]:
            compare(False, *args)
        for rule in ['+missing', '+@missing', '%RR~key', '%~key', '~new',
                     '#abc', '!ABC', '<missing', '-select|0',
                     '+client|id|arg', '(+get', '(on)', '&bad pattern']:
            compare(True, 'ACL', 'SETUSER', 'bob', rule)
        for args in [('bob', 'GET', 'key'), ('bob', 'SET', 'key', 'v'),
                     ('bob', 'CLIENT', 'GETNAME'), ('none', 'PING'),
                     ('bob', 'missing'), ('bob', 'GET'),
                     ('bob', 'client|id'),
                     ('bob', 'AUTH', 'default', 'x')]:
            compare(True, 'ACL', 'DRYRUN', *args)
        compare(True, 'ACL', 'SETUSER', 'bob', 'reset', 'on', 'nopass',
                '+@all', 'resetkeys', '%R~*', '%W~target:*')
        # Different overlapping patterns cannot combine R and W permissions.
        compare(False, 'SET', 'target:a', 'v', 'GET')
        compare(True, 'ACL', 'SETUSER', 'bob', '%R~target:*')
        compare(False, 'SET', 'target:a', 'v', 'GET')
        compare(True, 'ACL', 'SETUSER', 'bob', 'off')
        compare(False, 'PING')  # OFF preserves already authenticated sessions.
        compare(True, 'ACL', 'DELUSER', 'none', 'bob', 'bob')
        compare(True, 'ACL', 'GETUSER', 'bob')
        compare(True, 'ACL', 'DELUSER', 'default', 'none')
        return checks
    finally:
        for client in (a, b, x, y):
            client.close()


def credentials(port):
    admin, client = Client(port), Client(port)
    try:
        password = b'bin\0ary\xffpassword'
        digest = hashlib.sha256(password).hexdigest()
        assert admin.call('ACL', 'SETUSER', 'binary', 'on', '#' + digest,
                          '+get', '~*') == b'OK'
        assert client.call('AUTH', 'binary', password) == b'OK'
        assert isinstance(client.call('AUTH', 'binary', password + b'x'),
                          ErrorReply)
        assert client.call('GET', 'missing') is None
        assert admin.call('ACL', 'GETUSER', 'binary')[3] == [digest.encode()]
        assert admin.call('ACL', 'SETUSER', 'binary', '<' +
                          password.decode('latin1')) != b'OK'
        assert admin.call('ACL', 'SETUSER', 'binary', b'<' + password) == b'OK'
        assert isinstance(client.call('AUTH', 'binary', password), ErrorReply)
        assert client.call('GET', 'missing') is None
        before = admin.call('ACL', 'GETUSER', 'binary')
        assert isinstance(admin.call('ACL', 'SETUSER', 'binary', 'off',
                                     '+invalid-command'), ErrorReply)
        assert admin.call('ACL', 'GETUSER', 'binary') == before
        assert admin.call('ACL', 'SETUSER', 'binary', 'nopass') == b'OK'
        assert client.call('HELLO', 2, 'AUTH', 'binary', 'anything')
        # Switch back using a RESP3-aware reader on a separate connection.
        c = RespClient(port)
        try:
            assert c.call('HELLO', 3, 'AUTH', 'binary', 'any').kind == b'%'
            assert c.call('GET', 'missing').kind == b'_'
        finally:
            c.close()
        assert admin.call('ACL', 'SETUSER', 'binary', '+client|info') == b'OK'
        c = Client(port)
        try:
            assert c.call('AUTH', 'binary', 'x') == b'OK'
            assert b'user=binary ' in c.call('CLIENT', 'INFO')
        finally:
            c.close()
    finally:
        admin.close()
        client.close()


def transactions(port, reference):
    for selected in (port, reference):
        admin, client = Client(selected), Client(selected)
        try:
            assert admin.call('ACL', 'SETUSER', 'tx', 'on', 'nopass',
                              '+@all', '~allowed:*') == b'OK'
            assert client.call('AUTH', 'tx', 'x') == b'OK'
            assert client.call('MULTI') == b'OK'
            assert isinstance(client.call('SET', 'denied', 'v'), ErrorReply)
            assert isinstance(client.call('EXEC'), ErrorReply)
            assert admin.call('GET', 'denied') is None
            assert client.call('MULTI') == b'OK'
            assert client.call('SET', 'allowed:a', 'first') == b'QUEUED'
            assert client.call('GET', 'allowed:a') == b'QUEUED'
            assert admin.call('ACL', 'SETUSER', 'tx', '-set') == b'OK'
            result = client.call('EXEC')
            assert len(result) == 2 and isinstance(result[0], ErrorReply)
            assert result[0].startswith(b'NOPERM ACLs rules changed')
            assert result[1] is None and admin.call('GET', 'allowed:a') is None
            assert admin.call('ACL', 'SETUSER', 'tx', '+set') == b'OK'
            assert client.call('MULTI') == b'OK'
            assert client.call('ACL', 'SETUSER', 'tx', '-set') == b'QUEUED'
            assert client.call('SET', 'allowed:a', 'never') == b'QUEUED'
            result = client.call('EXEC')
            assert result[0] == b'OK' and isinstance(result[1], ErrorReply)
            assert admin.call('GET', 'allowed:a') is None
        finally:
            admin.close()
            client.close()


def default_sessions(port):
    admin, existing, fresh = Client(port), Client(port), Client(port)
    try:
        assert admin.call('ACL', 'SETUSER', 'operator', 'on', '>root-secret',
                          '+@all', '~*', '&*') == b'OK'
        assert admin.call('AUTH', 'operator', 'root-secret') == b'OK'
        assert admin.call('ACL', 'SETUSER', 'default', 'off', 'resetpass') == \
            b'OK'
        assert existing.call('PING') == b'PONG'
        assert fresh.call('RESET') == b'RESET'
        assert fresh.call('PING') == ErrorReply(b'NOAUTH Authentication '
                                              b'required.')
        assert isinstance(fresh.call('AUTH', 'default', 'anything'), ErrorReply)
        assert fresh.call('HELLO', 2, 'AUTH', 'operator', 'root-secret')
        assert b'user=operator ' in fresh.call('CLIENT', 'INFO')
        assert fresh.call('RESET') == b'RESET'
        assert isinstance(fresh.call('PING'), ErrorReply)
        newest = Client(port)
        try:
            assert isinstance(newest.call('PING'), ErrorReply)
        finally:
            newest.close()
        assert admin.call('ACL', 'SETUSER', 'default', 'on', 'nopass') == b'OK'
        value = fresh.call('PING')
        assert value == b'PONG', value
        assert isinstance(fresh.call('HELLO', 2), ErrorReply)
        assert fresh.call('MULTI') == b'OK'
        assert fresh.call('SET', 'implicit-write', 'never') == b'QUEUED'
        assert admin.call('ACL', 'SETUSER', 'default', 'resetpass',
                          '>new-secret') == b'OK'
        assert isinstance(fresh.call('PING'), ErrorReply)
        result = fresh.call('EXEC')
        assert isinstance(result, ErrorReply) and \
            result.startswith(b'EXECABORT')
        assert admin.call('GET', 'implicit-write') is None
        assert fresh.call('AUTH', 'default', 'new-secret') == b'OK'
        assert admin.call('ACL', 'SETUSER', 'default', 'nopass') == b'OK'
        assert fresh.call('RESET') == b'RESET'
        assert fresh.call('ACL', 'WHOAMI') == b'default'
        assert fresh.call('PING') == b'PONG'
    finally:
        admin.close()
        existing.close()
        fresh.close()


def revocation(port):
    admin, client = Client(port), Client(port)
    try:
        assert admin.call('ACL', 'SETUSER', 'victim', 'on', 'nopass',
                          '+@all', '~*') == b'OK'
        assert client.call('AUTH', 'victim', 'any') == b'OK'
        assert admin.call('CLIENT', 'KILL', 'USER', 'victim') == 1
        assert client.socket.recv(1) == b''
        client.close()
        client = Client(port)
        assert client.call('AUTH', 'victim', 'any') == b'OK'
        assert admin.call('ACL', 'DELUSER', 'victim') == 1
        assert client.socket.recv(1) == b''
        assert admin.call('ACL', 'SETUSER', 'victim', 'on', 'nopass') == b'OK'
        fresh = Client(port)
        try:
            assert fresh.call('AUTH', 'victim', 'any') == b'OK'
            assert isinstance(fresh.call('GET', 'key'), ErrorReply)
            assert fresh.call('RESET') == b'RESET'
            assert fresh.call('ACL', 'WHOAMI') == b'default'
        finally:
            fresh.close()
    finally:
        admin.close()
        client.close()


def concurrent_permissions(port):
    admin = Client(port)
    try:
        assert admin.call('ACL', 'SETUSER', 'live', 'on', 'nopass',
                          '+get', '~one:*') == b'OK'
        assert admin.call('SET', 'forbidden', 'stable') == b'OK'
        barrier = threading.Barrier(9)

        def writer():
            c = Client(port)
            try:
                barrier.wait(timeout=10)
                for step in range(128):
                    rules = ('+get', '~one:*') if step % 2 else \
                        ('+set', '~two:*')
                    assert c.call('ACL', 'SETUSER', 'live', 'reset', 'on',
                                  'nopass', *rules) == b'OK'
            finally:
                c.close()

        def reader(_):
            c = Client(port)
            try:
                assert c.call('AUTH', 'live', 'anything') == b'OK'
                barrier.wait(timeout=10)
                for step in range(64):
                    value = c.call('SET', 'forbidden', 'never')
                    assert isinstance(value, ErrorReply), value
                    value = c.call('GET', 'one:a')
                    assert value is None or isinstance(value, ErrorReply)
                    value = c.call('SET', 'two:a', step)
                    assert value == b'OK' or isinstance(value, ErrorReply)
            finally:
                c.close()

        with ThreadPoolExecutor(max_workers=9) as pool:
            future = pool.submit(writer)
            list(pool.map(reader, range(8)))
            future.result()
        assert admin.call('GET', 'forbidden') == b'stable'
    finally:
        admin.close()


def typed_and_random(port):
    client = RespClient(port)
    try:
        assert client.call('HELLO', 3).kind == b'%'
        user = mapping(client.call('ACL', 'GETUSER', 'default'))
        assert user[b'flags'].kind == b'~' and user[b'selectors'].kind == b'*'
        assert user[b'commands'].value == b'+@all'
        values = [client.call('ACL', 'GENPASS').value for _ in range(64)]
        assert len(set(values)) == len(values)
        for bits in (1, 4, 5, 255, 256, 4096):
            result = client.call('ACL', 'GENPASS', bits)
            assert len(result.value) == (bits + 3) // 4
            assert all(ch in b'0123456789abcdef' for ch in result.value)
        for bits in (0, -1, 4097, 'bad'):
            assert client.call('ACL', 'GENPASS', bits).kind == b'-'
        assert client.call('ACL', 'GETUSER', 'absent').kind == b'_'
    finally:
        client.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--pkv', required=True)
    parser.add_argument('--redis', required=True)
    args = parser.parse_args()
    with server(args.redis, redis=True) as reference, \
            server(args.pkv) as port:
        count = differential(port, reference)
        credentials(port)
        transactions(port, reference)
        default_sessions(port)
        default_sessions(reference)
        revocation(port)
        typed_and_random(port)
        concurrent_permissions(port)
    print('ACL stable differential assertions:', count)
    print('ACL credentials, selectors, transactions, revocation and RESP3 '
          'passed')


if __name__ == '__main__':
    main()
