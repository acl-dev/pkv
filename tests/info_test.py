#!/usr/bin/env python3
"""INFO real counters, command/error statistics, reset and keyspace metadata."""
import argparse
import os
import re
import shutil
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor

from integration_test import Client, ErrorReply, server
from resp3_client import RespClient


def parse(raw):
    assert isinstance(raw, bytes)
    sections, result = [], {}
    for line in raw.split(b'\r\n'):
        if not line:
            continue
        if line.startswith(b'# '):
            sections.append(line[2:])
        else:
            key, value = line.split(b':', 1)
            assert key not in result, key
            result[key] = value
    return sections, result


def info(client, *sections):
    return parse(client.call('INFO', *sections))


def fields(value):
    return dict(part.split(b'=', 1) for part in value.split(b','))


def stable(client):
    _, data = info(client, 'stats', 'commandstats', 'errorstats')
    output = {key: int(data[key]) for key in [b'total_commands_processed',
        b'total_error_replies', b'total_connections_received',
        b'acl_access_denied_auth', b'acl_access_denied_cmd',
        b'acl_access_denied_key']}
    for key, value in data.items():
        if key.startswith(b'cmdstat_'):
            metric = fields(value)
            output[key] = tuple(int(metric[name]) for name in
                [b'calls', b'rejected_calls', b'failed_calls'])
            assert int(metric[b'usec']) >= 0
            assert float(metric[b'usec_per_call']) >= 0
        elif key.startswith(b'errorstat_'):
            output[key] = int(fields(value)[b'count'])
    return output


def observations(binary, reference):
    snapshots = []
    with server(binary, redis=reference, active_expire=False) as port:
        admin, client = Client(port), Client(port)
        try:
            assert client.call('PING') == b'PONG'
            assert admin.call('CONFIG', 'RESETSTAT') == b'OK'
            for command in [('PING',), ('SET', 'x', 'value'), ('GET', 'x'),
                            ('LPUSH', 'x', 'no'), ('GET',), ('not-a-command',)]:
                admin.call(*command)
                snapshots.append(stable(admin))
            assert admin.call('CONFIG', 'RESETSTAT') == b'OK'
            snapshots.append(stable(admin))
            assert admin.call('MULTI') == b'OK'
            assert admin.call('LPUSH', 'x', 'no') == b'QUEUED'
            assert admin.call('GET', 'x') == b'QUEUED'
            snapshots.append(stable(client))
            result = admin.call('EXEC')
            assert isinstance(result[0], ErrorReply) and result[1] == b'value'
            snapshots.append(stable(admin))
            assert admin.call('ACL', 'SETUSER', 'reader', 'on', 'nopass',
                              '+get', '+multi', '+exec', '~own:*') == b'OK'
            assert client.call('AUTH', 'reader', 'unused') == b'OK'
            assert isinstance(client.call('GET', 'x'), ErrorReply)
            assert isinstance(client.call('SET', 'own:x', 'no'), ErrorReply)
            assert isinstance(client.call('AUTH', 'ghost', 'wrong'), ErrorReply)
            snapshots.append(stable(admin))
            assert client.call('MULTI') == b'OK'
            assert client.call('GET', 'own:x') == b'QUEUED'
            assert admin.call('ACL', 'SETUSER', 'reader', '-get') == b'OK'
            result = client.call('EXEC')
            assert len(result) == 1 and isinstance(result[0], ErrorReply)
            snapshots.append(stable(admin))
            assert admin.call('CONFIG', 'RESETSTAT') == b'OK'
            snapshots.append(stable(admin))
            assert admin.call('MULTI') == b'OK'
            for command in [('PING',), ('CONFIG', 'RESETSTAT'), ('PING',),
                            ('INFO', 'stats')]:
                assert admin.call(*command) == b'QUEUED'
            result = admin.call('EXEC')
            assert result[:3] == [b'PONG', b'OK', b'PONG']
            assert int(parse(result[3])[1][b'total_commands_processed']) == 2
            snapshots.append(stable(admin))
        finally:
            admin.close()
            client.close()
    return snapshots


def sections_and_keyspace(binary, reference):
    with server(binary, redis=reference, active_expire=False) as port:
        client = Client(port)
        try:
            _, identity = info(client, 'server')
            assert re.fullmatch(b'[0-9a-f]{40}', identity[b'run_id'])
            assert int(identity[b'process_id']) > 0
            assert int(identity[b'tcp_port']) == port
            for selected in [('server',), ('SeRvEr', 'server'),
                             ('keyspace', 'clients'), ('bad-section',),
                             (b'server\0ignored',)]:
                found, _ = info(client, *selected)
                assert len(found) == len(set(found))
                if selected == ('bad-section',):
                    assert found == []
            assert info(client, 'keyspace')[1] == {}
            assert client.call('SET', 'permanent', 'value') == b'OK'
            assert client.call('SET', 'temporary', 'value',
                               'PX', 60000) == b'OK'
            assert client.call('LPUSH', 'list', 'a', 'b', 'c') == 3
            assert client.call('SELECT', 3) == b'OK'
            assert client.call('HSET', 'map', 'field', 'value') == 1
            assert client.call('SELECT', 0) == b'OK'
            _, data = info(client, 'keyspace')
            assert set(data) == {b'db0', b'db3'}
            assert fields(data[b'db0'])[b'keys'] == b'3'
            assert fields(data[b'db0'])[b'expires'] == b'1'
            assert fields(data[b'db3'])[b'keys'] == b'1'
            assert fields(data[b'db3'])[b'expires'] == b'0'
            assert client.call('MULTI') == b'OK'
            assert client.call('SET', 'inside-exec', 'value') == b'QUEUED'
            assert client.call('INFO', 'keyspace') == b'QUEUED'
            result = client.call('EXEC')
            assert result[0] == b'OK'
            assert fields(parse(result[1])[1][b'db0'])[b'keys'] == b'4'
            _, memory = info(client, 'memory')
            if b'used_memory' in memory:
                assert int(memory[b'used_memory']) > 0
            assert int(memory[b'used_memory_rss']) > 0
            _, cpu = info(client, 'cpu')
            assert float(cpu[b'used_cpu_user']) >= 0
            typed = RespClient(port)
            try:
                assert typed.call('HELLO', 3).kind == b'%'
                response = typed.call('INFO', 'keyspace')
                assert response.kind == b'=' and response.value.startswith(
                    b'txt:# Keyspace\r\n')
            finally:
                typed.close()
            assert info(client, 'server')[1][b'run_id'] == identity[b'run_id']
        finally:
            client.close()


def concurrent(binary):
    with server(binary) as port:
        admin = Client(port)
        try:
            assert admin.call('CONFIG', 'RESETSTAT') == b'OK'

            def worker(_):
                client = Client(port)
                try:
                    for _ in range(100):
                        assert client.call('PING') == b'PONG'
                finally:
                    client.close()
            with ThreadPoolExecutor(max_workers=8) as pool:
                list(pool.map(worker, range(8)))
            data = stable(admin)
            assert data[b'cmdstat_ping'] == (800, 0, 0)
            assert data[b'total_connections_received'] == 8
            _, clients = info(admin, 'clients')
            deadline = time.monotonic() + 3
            while int(clients[b'connected_clients']) != 1:
                assert time.monotonic() < deadline
                _, clients = info(admin, 'clients')
            assert stable(admin)[b'cmdstat_ping'] == (800, 0, 0)
        finally:
            admin.close()


def persistent_keyspace(binary):
    if os.environ.get('PKV_TEST_BACKEND', 'mdb') not in ('rdb', 'wdb'):
        return
    with tempfile.TemporaryDirectory(prefix='pkv-info-restart-') as path:
        with server(binary, storage_path=path) as port:
            client = Client(port)
            try:
                assert client.call('SELECT', 3) == b'OK'
                assert client.call('SET', 'persisted', 'yes') == b'OK'
            finally:
                client.close()
        with server(binary, storage_path=path) as port:
            client = Client(port)
            try:
                data = info(client, 'keyspace')[1]
                assert set(data) == {b'db3'}, data
                assert fields(data[b'db3'])[b'keys'] == b'1'
                assert client.call('SELECT', 3) == b'OK'
                assert client.call('GET', 'persisted') == b'yes'
            finally:
                client.close()
        stored = os.path.join(path, 'logical-db-3')
        shutil.rmtree(stored)
        with open(stored, 'wb') as corrupt:
            corrupt.write(b'not a database directory')
        with server(binary, storage_path=path) as port:
            client = Client(port)
            try:
                result = client.call('INFO', 'keyspace')
                assert isinstance(result, ErrorReply), result
                assert result.startswith(b'ERR keyspace statistics')
                assert client.call('PING') == b'PONG'
            finally:
                client.close()


def network_bytes(binary):
    with server(binary) as port:
        client = Client(port)
        try:
            assert client.call('CONFIG', 'RESETSTAT') == b'OK'
            data = info(client, 'stats')[1]
            assert int(data[b'total_net_input_bytes']) == len(
                Client.encode(('INFO', 'stats')))
            assert int(data[b'total_net_output_bytes']) == len(b'+OK\r\n')
            # All pipeline input arrived before the reset. Output is flushed
            # after the entire parsed pipeline, including INFO, is executed.
            client.socket.sendall(b''.join(Client.encode(command) for command
                in [('CONFIG', 'RESETSTAT'), ('PING',), ('INFO', 'stats')]))
            assert client.read() == b'OK'
            assert client.read() == b'PONG'
            data = parse(client.read())[1]
            assert int(data[b'total_net_input_bytes']) == 0
            assert int(data[b'total_net_output_bytes']) == 0
        finally:
            client.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--pkv', required=True)
    parser.add_argument('--redis', required=True)
    args = parser.parse_args()
    actual = observations(args.pkv, False)
    expected = observations(args.redis, True)
    for index, (found, wanted) in enumerate(zip(actual, expected)):
        assert found == wanted, (index, {
            key: (found.get(key), wanted.get(key))
            for key in found.keys() | wanted.keys()
            if found.get(key) != wanted.get(key)})
    assert len(actual) == len(expected)
    sections_and_keyspace(args.pkv, False)
    sections_and_keyspace(args.redis, True)
    concurrent(args.pkv)
    persistent_keyspace(args.pkv)
    network_bytes(args.pkv)
    print('INFO stable counter observations:', len(actual))
    print('INFO sections, reset, keyspace, protocols and '
          'concurrent totals passed')


if __name__ == '__main__':
    main()
