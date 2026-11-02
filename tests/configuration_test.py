#!/usr/bin/env python3
"""CONFIG values, validation, atomic updates and live authorization."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import threading

from integration_test import Client, ErrorReply, server
from resp3_client import RespClient

NAMES = {b'aclfile', b'acllog-max-len', b'appendonly', b'cluster-enabled',
         b'databases', b'requirepass', b'save', b'replica-read-only',
         b'slave-read-only'}


def get(client, *patterns):
    result = client.call('CONFIG', 'GET', *patterns)
    assert isinstance(result, list) and len(result) % 2 == 0, result
    values = dict(zip(result[::2], result[1::2]))
    assert len(values) * 2 == len(result), 'duplicate result fields'
    return values


def observe(binary, reference):
    output = []
    with server(binary, redis=reference) as port:
        admin = Client(port)
        try:
            for patterns in [('*',), ('acl*',), ('ACL*',), ('REQUIREPASS',),
                             (b'ACL*\0tail',),
                             ('',), ('unknown',), ('*pass', '*len', 'save'),
                             ('REQUIREPASS', '*', 'requirepass'),
                             ('requirepass', 'requirepass', '*pass')]:
                values = get(admin, *patterns)
                output.append({k: v for k, v in values.items()
                               if k.lower() in NAMES})
                if not reference:
                    assert not {k.lower() for k in values} - NAMES
            for command in [('CONFIG',), ('CONFIG', 'GET'), ('CONFIG', 'SET'),
                            ('CONFIG', 'SET', 'requirepass'),
                            ('CONFIG', 'SET', 'requirepass', 'new', 'orphan'),
                            ('CONFIG', 'HELP', 'extra'),
                            ('CONFIG', 'bad')]:
                result = admin.call(*command)
                assert isinstance(result, ErrorReply), (command, result)
                output.append(result)
            for pairs in [('unknown', '1'), ('databases', '16'),
                          ('aclfile', ''), ('cluster-enabled', 'no'),
                          ('acllog-max-len', '-1'), ('acllog-max-len', '01'),
                          ('acllog-max-len', '1.0'),
                          ('acllog-max-len', '9223372036854775808'),
                          ('acllog-max-len', 'bad', 'unknown', '1'),
                          ('requirepass', 'new', 'requirepass', 'other'),
                          ('requirepass', 'new', 'acllog-max-len', 'bad'),
                          ('appendonly', 'bad')]:
                before = get(admin, '*')
                result = admin.call('CONFIG', 'SET', *pairs)
                assert isinstance(result, ErrorReply), (pairs, result)
                assert get(admin, '*') == before, 'partial CONFIG SET'
                output.append(result)
            assert admin.call('CONFIG', 'SET', 'save', '',
                              'appendonly', 'nO') == b'OK'
            if not reference:
                for name, value in [('appendonly', 'yes'), ('save', '60 1')]:
                    before = get(admin, '*')
                    assert isinstance(admin.call('CONFIG', 'SET', 'requirepass',
                                                 'never', name, value),
                                      ErrorReply)
                    assert get(admin, '*') == before
            assert admin.call('CONFIG', 'SET', 'REQUIREPASS', 'first',
                              'ACLLOG-MAX-LEN', '2') == b'OK'
            output.append(get(admin, 'requirepass', 'acllog-max-len'))
            assert admin.call('ACL', 'SETUSER', 'default', 'resetpass',
                              '>acl-pass') == b'OK'
            assert admin.call('CONFIG', 'SET', 'requirepass', 'first') == b'OK'
            client = Client(port)
            try:
                assert isinstance(client.call('AUTH', 'first'), ErrorReply)
                assert client.call('AUTH', 'acl-pass') == b'OK'
            finally:
                client.close()
            password = b'binary\0secret'
            assert admin.call('CONFIG', 'SET', 'requirepass', password,
                              'acllog-max-len', '0') == b'OK'
            client = Client(port)
            try:
                assert isinstance(client.call('PING'), ErrorReply)
                assert client.call('AUTH', password) == b'OK'
                assert client.call('PING') == b'PONG'
            finally:
                client.close()
            output.append(get(admin, 'requirepass', 'acllog-max-len'))
            typed = RespClient(port)
            try:
                assert typed.call('HELLO', 3, 'AUTH', 'default',
                                  password).kind == b'%'
                result = typed.call('CONFIG', 'GET', '*pass', 'acl*')
                assert result.kind == b'%'
                assert all(item.kind == b'$' for item in result.value)
            finally:
                typed.close()
            assert admin.call('CONFIG', 'SET', 'requirepass', '') == b'OK'
        finally:
            admin.close()
    return output


def transactions(binary, reference):
    with server(binary, redis=reference, password='start') as port:
        admin, anonymous = Client(port), Client(port)
        try:
            assert admin.call('AUTH', 'start') == b'OK'
            assert isinstance(anonymous.call('PING'), ErrorReply)
            assert admin.call('CONFIG', 'SET', 'requirepass', '') == b'OK'
            assert anonymous.call('PING') == b'PONG'
            assert admin.call('CONFIG', 'SET', 'requirepass', 'next') == b'OK'
            assert isinstance(anonymous.call('PING'), ErrorReply)
            assert admin.call('ACL', 'SETUSER', 'reader', 'on', 'nopass',
                              '+config|get') == b'OK'
            assert anonymous.call('AUTH', 'reader', 'unused') == b'OK'
            assert get(anonymous, 'acllog-max-len') == {
                b'acllog-max-len': b'128'}
            assert isinstance(anonymous.call('CONFIG', 'SET',
                                             'acllog-max-len', '2'), ErrorReply)
            assert admin.call('MULTI') == b'OK'
            assert admin.call('SET', 'before-config', 'yes') == b'QUEUED'
            assert admin.call('CONFIG', 'SET', 'acllog-max-len', '2',
                              'requirepass', 'exec') == b'QUEUED'
            assert admin.call('CONFIG', 'GET', 'requirepass') == b'QUEUED'
            result = admin.call('EXEC')
            assert result == [b'OK', b'OK', [b'requirepass', b'exec']]
            assert admin.call('GET', 'before-config') == b'yes'
            assert admin.call('MULTI') == b'OK'
            assert admin.call('CONFIG', 'SET', 'acllog-max-len', '3',
                              'requirepass', 'bad', 'unknown', 'x') == b'QUEUED'
            assert admin.call('CONFIG', 'GET', 'requirepass') == b'QUEUED'
            result = admin.call('EXEC')
            assert isinstance(result[0], ErrorReply)
            assert result[1] == [b'requirepass', b'exec']
            assert get(admin, 'acllog-max-len') == {b'acllog-max-len': b'2'}
        finally:
            admin.close()
            anonymous.close()


def runtime_log(binary, reference):
    with server(binary, redis=reference) as port:
        admin, client = Client(port), Client(port)
        try:
            assert admin.call('ACL', 'SETUSER', 'a', 'on', 'nopass',
                              '+get') == b'OK'
            assert client.call('AUTH', 'a', 'unused') == b'OK'
            for i in range(5):
                assert isinstance(client.call('GET', str(i)), ErrorReply)
            assert admin.call('CONFIG', 'SET', 'acllog-max-len', '2') == b'OK'
            assert len(admin.call('ACL', 'LOG', 100)) == 5
            assert isinstance(client.call('GET', '4'), ErrorReply)
            assert len(admin.call('ACL', 'LOG', 100)) == 5
            assert isinstance(client.call('GET', 'trigger'), ErrorReply)
            assert len(admin.call('ACL', 'LOG', 100)) == 2
            assert admin.call('CONFIG', 'SET', 'acllog-max-len', '0') == b'OK'
            assert len(admin.call('ACL', 'LOG', 100)) == 2
            assert isinstance(client.call('GET', 'ignored'), ErrorReply)
            assert admin.call('ACL', 'LOG') == []
            assert admin.call('CONFIG', 'SET', 'acllog-max-len', '1') == b'OK'
            assert isinstance(client.call('GET', 'again'), ErrorReply)
            assert len(admin.call('ACL', 'LOG')) == 1
        finally:
            admin.close()
            client.close()


def race(binary):
    with server(binary) as port:
        admin = Client(port)
        try:
            ready = threading.Barrier(5)

            def reader(_):
                client = Client(port)
                try:
                    assert client.call('PING') == b'PONG'
                    ready.wait(timeout=10)
                    for _ in range(150):
                        values = get(client, 'requirepass', 'acllog-max-len')
                        assert values in [
                            {b'requirepass': b'', b'acllog-max-len': b'128'},
                            {b'requirepass': b'a', b'acllog-max-len': b'1'},
                            {b'requirepass': b'b', b'acllog-max-len': b'2'}]
                finally:
                    client.close()
            # Open/authenticate readers while the default user is still nopass.
            def writer():
                for i in range(100):
                    assert admin.call('CONFIG', 'SET', 'requirepass',
                                      'a' if i % 2 else 'b', 'acllog-max-len',
                                      '1' if i % 2 else '2') == b'OK'
            with ThreadPoolExecutor(max_workers=4) as pool:
                jobs = [pool.submit(reader, i) for i in range(4)]
                ready.wait(timeout=10)
                writer()
                for job in jobs:
                    job.result()
        finally:
            admin.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--pkv', required=True)
    parser.add_argument('--redis', required=True)
    args = parser.parse_args()
    actual, expected = observe(args.pkv, False), observe(args.redis, True)
    assert actual == expected, (actual, expected)
    transactions(args.pkv, False)
    transactions(args.redis, True)
    runtime_log(args.pkv, False)
    runtime_log(args.redis, True)
    race(args.pkv)
    print('CONFIG common observations:', len(actual))
    print('CONFIG live values, atomicity, ACL, protocols and EXEC passed')


if __name__ == '__main__':
    main()
