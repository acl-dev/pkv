#!/usr/bin/env python3
"""ACL SAVE/LOAD, startup/restart, atomic failures and live identity updates."""
import argparse
import hashlib
from pathlib import Path
import tempfile

from integration_test import Client, ErrorReply, server
from resp3_client import RespClient

ADMIN = 'owner-secret'
NEXT = 'replacement-secret'
TRANSPORT = 'transport-secret'


def initial():
    return ('user default off resetpass -@all\n'
            'user owner on >%s ~* &* +@all\n'
            'user app on >app-secret ~app:* +get +set +client|id '
            '+client|info\n') % ADMIN


def authenticate(port, user='owner', password=ADMIN):
    client = Client(port)
    assert client.call('AUTH', user, password) == b'OK'
    return client


def commands_without_file(binary, reference):
    with server(binary) as p, server(reference, redis=True) as r:
        a, b = Client(p), Client(r)
        try:
            for command in [('ACL', 'LOAD'), ('ACL', 'SAVE'),
                            ('ACL', 'LOAD', 'extra'),
                            ('ACL', 'SAVE', 'extra')]:
                x, y = a.call(*command), b.call(*command)
                assert x == y and isinstance(x, ErrorReply), (command, x, y)
        finally:
            a.close()
            b.close()


def workflow(binary, reference, path):
    path.write_text(initial())
    with server(binary, redis=reference, aclfile=path, password=TRANSPORT,
                login=('owner', ADMIN)) as port:
        owner = authenticate(port)
        app = authenticate(port, 'app', 'app-secret')
        fresh = None
        try:
            assert owner.call('ACL', 'WHOAMI') == b'owner'
            assert isinstance(client_call(port, 'PING'), ErrorReply)
            identity = app.call('CLIENT', 'ID')
            assert app.call('SET', 'app:key', 'value') == b'OK'
            secret = b'fresh\0password'
            assert owner.call('ACL', 'SETUSER', 'fresh', 'on', b'>' + secret,
                              '+get', '~*') == b'OK'
            fresh = authenticate(port, 'fresh', secret)
            assert owner.call('ACL', 'SETUSER', 'auditor', 'on', 'nopass',
                              '+@all', '-@admin', '~*', '&*') == b'OK'
            assert owner.call('ACL', 'SAVE') == b'OK'
            saved = path.read_bytes()
            assert ADMIN.encode() not in saved and \
                TRANSPORT.encode() not in saved
            assert secret not in saved
            assert hashlib.sha256(secret).hexdigest().encode() in saved
            if not reference:
                assert b'-@admin' in saved
                assert path.stat().st_mode & 0o777 == 0o600
            path.write_text('user default off -@all\n'
                            'user owner on >%s ~* &* +@all\n'
                            'user app off >changed ~app:* +get '
                            '+client|id +client|info\n' % NEXT)
            assert owner.call('ACL', 'LOAD') == b'OK'
            assert fresh.socket.recv(1) == b''
            assert app.call('CLIENT', 'ID') == identity
            assert app.call('GET', 'app:key') == b'value'
            assert isinstance(app.call('SET', 'app:key', 'never'), ErrorReply)
            assert owner.call('ACL', 'WHOAMI') == b'owner'
            attempt = Client(port)
            try:
                assert isinstance(attempt.call('AUTH', 'owner', ADMIN),
                                  ErrorReply)
                assert attempt.call('AUTH', 'owner', NEXT) == b'OK'
                assert isinstance(attempt.call('AUTH', 'app', 'changed'),
                                  ErrorReply)
            finally:
                attempt.close()
            before = owner.call('ACL', 'LIST')
            invalid = ['user owner off +unknown-command\n',
                       'user duplicate on\nuser duplicate off\n', '# comment\n']
            # The local 8.6.2 reference crashes in ACLLoadFromFile for an
            # unmatched selector. Its standalone crash probe is retained;
            # PKV must reject it without changing the live policy.
            if not reference:
                invalid.append('user owner on (+get\n')
            for bad in invalid:
                path.write_text(bad)
                assert isinstance(owner.call('ACL', 'LOAD'), ErrorReply)
                assert owner.call('ACL', 'LIST') == before
                assert app.call('GET', 'app:key') == b'value'
            path.unlink()
            assert isinstance(owner.call('ACL', 'LOAD'), ErrorReply)
            assert owner.call('ACL', 'LIST') == before
            assert owner.call('ACL', 'SAVE') == b'OK'
            typed = RespClient(port)
            try:
                assert typed.call('HELLO', 3, 'AUTH', 'owner',
                                  NEXT).kind == b'%'
                assert typed.call('ACL', 'LOAD') == (b'+', b'OK')
                assert typed.call('ACL', 'SAVE') == (b'+', b'OK')
            finally:
                typed.close()
        finally:
            owner.close()
            app.close()
            if fresh is not None:
                fresh.close()
    with server(binary, redis=reference, aclfile=path,
                login=('owner', NEXT)) as port:
        owner = authenticate(port, password=NEXT)
        try:
            assert owner.call('ACL', 'WHOAMI') == b'owner'
            assert isinstance(client_call(port, 'PING'), ErrorReply)
            assert owner.call('ACL', 'GETUSER', 'fresh') is None
            assert owner.call('ACL', 'GETUSER', 'app')[1][0] == b'off'
        finally:
            owner.close()


def client_call(port, *args):
    client = Client(port)
    try:
        return client.call(*args)
    finally:
        client.close()


def fallback(binary, reference, path):
    path.write_text('user app on nopass +ping\n')
    with server(binary, redis=reference, aclfile=path, password=TRANSPORT,
                login=('default', 'anything')) as port:
        client = Client(port)
        try:
            assert client.call('PING') == b'PONG'
            assert client.call('ACL', 'WHOAMI') == b'default'
        finally:
            client.close()


def invalid_startup(binary, reference, path):
    for content in ('user bad on +unknown-command\n', None):
        if content is None:
            path.unlink(missing_ok=True)
        else:
            path.write_text(content)
        rejected = False
        try:
            with server(binary, redis=reference, aclfile=path):
                raise AssertionError('invalid ACL started serving')
        except AssertionError as error:
            assert 'server exited with' in str(error), error
            rejected = True
        assert rejected


def quoted_and_self(binary, path):
    path.write_text(initial())
    with server(binary, aclfile=path, login=('owner', ADMIN)) as port:
        owner = authenticate(port)
        try:
            assert owner.call('ACL', 'SETUSER', 'quote"user', 'on', 'nopass',
                              '+get', '~quote"key',
                              '(+set "~own\\\\key")') == b'OK'
            assert owner.call('ACL', 'SAVE') == b'OK'
            expected = owner.call('ACL', 'GETUSER', 'quote"user')
            assert owner.call('ACL', 'LOAD') == b'OK'
            assert owner.call('ACL', 'GETUSER', 'quote"user') == expected
            path.write_text('user default off -@all\n'
                            'user replacement on >new ~* &* +@all\n')
            assert owner.call('MULTI') == b'OK'
            assert owner.call('SET', 'before-load', 'yes') == b'QUEUED'
            assert owner.call('ACL', 'LOAD') == b'QUEUED'
            assert owner.call('SET', 'after-load', 'never') == b'QUEUED'
            result = owner.call('EXEC')
            assert result[:2] == [b'OK', b'OK']
            assert isinstance(result[2], ErrorReply)
            assert owner.socket.recv(1) == b''
            replacement = authenticate(port, 'replacement', 'new')
            try:
                assert replacement.call('GET', 'before-load') == b'yes'
                assert replacement.call('GET', 'after-load') is None
            finally:
                replacement.close()
        finally:
            owner.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--pkv', required=True)
    parser.add_argument('--redis', required=True)
    args = parser.parse_args()
    commands_without_file(args.pkv, args.redis)
    with tempfile.TemporaryDirectory(prefix='pkv-acl-persist-') as directory:
        root = Path(directory)
        workflow(args.pkv, False, root / 'pkv.acl')
        workflow(args.redis, True, root / 'redis.acl')
        fallback(args.pkv, False, root / 'fallback.acl')
        fallback(args.redis, True, root / 'reference-fallback.acl')
        invalid_startup(args.pkv, False, root / 'invalid.acl')
        invalid_startup(args.redis, True,
                        root / 'reference-invalid.acl')
        quoted_and_self(args.pkv, root / 'quoted.acl')
    print('ACL files: restart, LOAD atomicity, live sessions, '
          'identity closure, '
          'RESP3, missing default and startup rejection passed')


if __name__ == '__main__':
    main()
