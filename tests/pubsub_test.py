#!/usr/bin/env python3
"""Pub/Sub frames, lifecycle, permissions and concurrent delivery parity."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import socket
import tempfile
import threading
import time
from pathlib import Path

from integration_test import Client, server
from resp3_client import RespClient, canonical, mapping

CHECKS = 0
SAFE_CHECKS = 0


def equal(left, right, label):
    global CHECKS
    assert canonical(left) == canonical(right), (label, left, right)
    CHECKS += 1
    return left


class Pair:
    def __init__(self, ports, protocol=2):
        self.clients = [RespClient(port) for port in ports]
        if protocol == 3:
            for client in self.clients:
                assert mapping(client.call('HELLO', 3))[b'proto'].value == 3

    def close(self):
        for client in self.clients:
            client.close()

    def call(self, *args):
        values = [client.call(*args) for client in self.clients]
        return equal(*values, args)

    def subscription(self, command, *names):
        for client in self.clients:
            client.socket.sendall(Client.encode([command, *names]))
        for position in range(max(1, len(names))):
            equal(self.clients[0].read(), self.clients[1].read(),
                  (command, position))

    def messages(self, count, unordered=False):
        values = [[canonical(client.read()) for _ in range(count)]
                  for client in self.clients]
        if unordered:
            values = [sorted(items, key=repr) for items in values]
        assert values[0] == values[1], values
        return values


def wait_for(predicate, label):
    deadline = time.monotonic() + 5
    while not predicate():
        assert time.monotonic() < deadline, label
        time.sleep(.005)


def rows(reply):
    value = reply.value
    if reply.kind == b'=':
        assert value.startswith(b'txt:')
        value = value[4:]
    return [dict(part.split(b'=', 1) for part in line.split())
            for line in value.splitlines()]


def info(reply):
    return dict(line.split(b':', 1) for line in reply.value.splitlines()
                if line and not line.startswith(b'#'))


def drain(ports):
    for port in ports:
        client = RespClient(port)
        try:
            def empty():
                return (client.call('PUBSUB', 'CHANNELS').value == [] and
                        client.call('PUBSUB', 'NUMPAT').value == 0 and
                        client.call('PUBSUB', 'SHARDCHANNELS').value == [])
            wait_for(empty, 'subscription retirement')
        finally:
            client.close()


def basic(ports):
    global SAFE_CHECKS
    for protocol in [2, 3]:
        listeners, admin = Pair(ports, protocol), Pair(ports)
        try:
            listeners.subscription('SUBSCRIBE', b'a\0\xff', 'b', b'a\0\xff')
            listeners.subscription('PSUBSCRIBE', b'a*', '*')
            listeners.subscription('SSUBSCRIBE', b'a\0\xff')
            assert admin.call('PUBLISH', b'a\0\xff', b'm\0\xff').value == 3
            listeners.messages(3, unordered=True)
            assert admin.call('SPUBLISH', b'a\0\xff', 'shard').value == 1
            listeners.messages(1)
            assert admin.call('PUBSUB', 'NUMPAT').value == 2
            admin.call('PUBSUB', 'NUMSUB', b'a\0\xff', 'b', b'a\0\xff')
            admin.call('PUBSUB', 'SHARDNUMSUB', b'a\0\xff', 'b')
            for command in ['CHANNELS', 'SHARDCHANNELS']:
                values = [client.call('PUBSUB', command, 'a*')
                          for client in admin.clients]
                assert sorted(item.value for item in values[0].value) == \
                    sorted(item.value for item in values[1].value)
            listeners.call('PING')
            listeners.call('PING', b'\0x')
            if protocol == 2:
                commands = [('GET', 'x'), ('CLIENT', 'ID'),
                            ('CLIENT', 'LIST'), ('INFO',), ('HELLO', 3),
                            ('PUBLISH', 'a', 'bad'), ('PUBSUB', 'NUMPAT'),
                            ('MULTI',), ('ECHO', 'v'), ('COMMAND', 'COUNT')]
                for args in commands:
                    listeners.call(*args)
            else:
                listeners.call('GET', 'missing')
            listeners.subscription('UNSUBSCRIBE', b'a\0\xff', b'a\0\xff',
                                   'missing')
            listeners.subscription('PUNSUBSCRIBE', b'a*', '*')
            listeners.subscription('PUNSUBSCRIBE')
            listeners.subscription('SUNSUBSCRIBE', b'a\0\xff')
            listeners.subscription('SUNSUBSCRIBE')
            listeners.call('RESET')
            listeners.call('GET', 'missing')
            admin.call('PUBSUB', 'NUMSUB', 'b')
            for args in [('SUBSCRIBE',), ('PSUBSCRIBE',), ('SSUBSCRIBE',),
                         ('PUBLISH', 'a'), ('SPUBLISH', 'a', 'b', 'extra'),
                         ('PUBSUB',), ('PUBSUB', 'NUMPAT', 'extra'),
                         ('PUBSUB', 'CHANNELS', 'a', 'extra'),
                         ('PUBSUB', 'bad')]:
                listeners.call(*args)
            # The reference hashes the full name but compares C strings;
            # NUL suffix acceptance varies with its hash seed. Reject it.
            for child in ['channels', 'numpat', 'numsub', 'help',
                          'shardchannels', 'shardnumsub']:
                reply = listeners.clients[0].call('PUBSUB',
                    child.encode() + b'\0bad')
                assert reply.kind == b'-' and reply.value.startswith(
                    b'ERR unknown subcommand'), reply
                SAFE_CHECKS += 1
            reply = listeners.clients[0].call(b'SUBSCRIBE\0bad', 'ghost')
            assert reply.kind == b'-', reply
            SAFE_CHECKS += 1
            listeners.call('PING')
            for command in ['SUBSCRIBE', 'PSUBSCRIBE', 'SSUBSCRIBE',
                            'SUNSUBSCRIBE', 'PUBLISH', 'SPUBLISH']:
                args = [command, 'a', 'b']
                for query in ['GETKEYS', 'GETKEYSANDFLAGS']:
                    admin.call('COMMAND', query, *args)
        finally:
            listeners.close()
            admin.close()
        drain(ports)


def shard_only(ports):
    for protocol in [2, 3]:
        listener, admin = Pair(ports, protocol), Pair(ports)
        try:
            listener.subscription('SSUBSCRIBE', 'shard-a', 'shard-b',
                                  'shard-a')
            listener.call('PING')
            listener.subscription('UNSUBSCRIBE')
            listener.call('GET', 'still-subscribed' if protocol == 2 else
                          'resp3-allowed')
            assert admin.call('SPUBLISH', 'shard-a', 'only-shard').value == 1
            listener.messages(1)
            listener.subscription('SUNSUBSCRIBE', 'shard-a', 'shard-b')
            listener.call('PING')
            listener.call('GET', 'no-longer-subscribed')
        finally:
            listener.close()
            admin.close()
        drain(ports)


def shared_patterns(ports):
    first, second, admin = Pair(ports, 3), Pair(ports, 3), Pair(ports)
    try:
        first.subscription('PSUBSCRIBE', 'shared:*')
        second.subscription('PSUBSCRIBE', 'shared:*')
        assert admin.call('PUBSUB', 'NUMPAT').value == 1
        assert admin.call('PUBLISH', 'shared:a', 'two-clients').value == 2
        first.messages(1)
        second.messages(1)
        first.call('RESET')
        assert admin.call('PUBSUB', 'NUMPAT').value == 1
        second.call('RESET')
        assert admin.call('PUBSUB', 'NUMPAT').value == 0
    finally:
        for pair in [first, second, admin]:
            pair.close()
    drain(ports)


def descriptions(ports):
    listeners, admin, normal = Pair(ports, 3), Pair(ports), Pair(ports)
    try:
        listeners.call('CLIENT', 'SETNAME', 'pubsub-fixture')
        admin.call('CLIENT', 'SETNAME', 'pubsub-admin')
        normal.call('CLIENT', 'SETNAME', 'pubsub-normal')
        listeners.subscription('SUBSCRIBE', 'a', 'b')
        listeners.subscription('PSUBSCRIBE', 'a*')
        listeners.subscription('SSUBSCRIBE', 'a')
        for client in admin.clients:
            subscribed = rows(client.call('CLIENT', 'LIST', 'TYPE', 'pubsub'))
            assert len(subscribed) == 1, subscribed
            row = subscribed[0]
            assert (row[b'sub'], row[b'psub'], row[b'ssub'], row[b'resp']) == \
                (b'2', b'1', b'1', b'3'), row
            assert b'P' in row[b'flags']
            regular = rows(client.call('CLIENT', 'LIST', 'TYPE', 'normal'))
            names = {item[b'name'] for item in regular}
            assert b'pubsub-fixture' not in names and \
                {b'pubsub-admin', b'pubsub-normal'} <= names, regular
            stats = info(client.call('INFO', 'CLIENTS', 'STATS'))
            assert [int(stats[key]) for key in [b'pubsub_clients',
                    b'pubsub_channels', b'pubsub_patterns',
                    b'pubsubshard_channels']] == [1, 2, 1, 1], stats
        values = [rows(client.call('CLIENT', 'INFO'))[0]
                  for client in listeners.clients]
        for key in [b'flags', b'db', b'sub', b'psub', b'ssub', b'resp',
                    b'user', b'cmd']:
            assert values[0][key] == values[1][key], (key, values)
        sub_ids = [client.call('CLIENT', 'ID').value
                   for client in listeners.clients]
        regular_ids = [client.call('CLIENT', 'ID').value
                       for client in normal.clients]
        for client, sub_id, regular_id in zip(admin.clients, sub_ids,
                                             regular_ids):
            assert client.call('CLIENT', 'KILL', 'TYPE', 'normal',
                               'ID', sub_id).value == 0
            assert client.call('CLIENT', 'KILL', 'TYPE', 'pubsub',
                               'ID', regular_id).value == 0
        assert admin.call('CLIENT', 'KILL', 'TYPE', 'pubsub').value == 1
        for client in listeners.clients:
            assert client.stream.readline() == b''
        drain(ports)
        admin.call('PING')
        self_pair = Pair(ports, 3)
        try:
            self_pair.subscription('SUBSCRIBE', 'self-kill')
            assert self_pair.call('CLIENT', 'KILL', 'TYPE', 'pubsub',
                                  'SKIPME', 'NO').value == 1
            for client in self_pair.clients:
                assert client.stream.readline() == b''
        finally:
            self_pair.close()
        drain(ports)
    finally:
        for pair in [listeners, admin, normal]:
            pair.close()


def blocked(ports):
    listeners, admin = Pair(ports, 3), Pair(ports)
    try:
        identities = [client.call('CLIENT', 'ID').value
                      for client in listeners.clients]
        listeners.subscription('SUBSCRIBE', 'events')
        for command, producer in [(['BLPOP', 'waiting', 5],
                                   ['LPUSH', 'waiting', 'v']),
                                  (['BZPOPMIN', 'waiting-z', 5],
                                   ['ZADD', 'waiting-z', 1, 'm'])]:
            for client in listeners.clients:
                client.socket.sendall(Client.encode(command))
            before = []
            for client, identity in zip(admin.clients, identities):
                def waiting():
                    value = rows(client.call('CLIENT', 'LIST', 'ID',
                                             identity))[0]
                    if b'b' in value[b'flags']:
                        before.append(int(value[b'argv-mem']))
                        return True
                    return False
                wait_for(waiting, 'subscriber did not block')
            assert admin.call('PUBLISH', 'events', 'during-block').value == 1
            listeners.messages(1)
            for client, identity, allocated in zip(admin.clients, identities,
                                                   before):
                row = rows(client.call('CLIENT', 'LIST', 'ID', identity))[0]
                assert b'b' in row[b'flags'] and allocated > 0 and \
                    int(row[b'argv-mem']) == allocated, row
            admin.call(*producer)
            listeners.messages(1)
        listeners.call('QUIT')
        for client in listeners.clients:
            assert client.stream.readline() == b''
    finally:
        listeners.close()
        admin.close()
    drain(ports)


def database_and_protocol(ports):
    listeners, admin = Pair(ports), Pair(ports)
    try:
        listeners.call('SELECT', 2)
        listeners.subscription('SUBSCRIBE', 'all-db')
        admin.call('SELECT', 3)
        assert admin.call('PUBLISH', 'all-db', 'db3').value == 1
        listeners.messages(1)
        listeners.call('RESET')
        for client in listeners.clients:
            assert mapping(client.call('HELLO', 3))[b'proto'].value == 3
        listeners.subscription('SUBSCRIBE', 'all-db')
        for client in listeners.clients:
            assert mapping(client.call('HELLO', 2))[b'proto'].value == 2
        listeners.call('GET', 'mode-restricted')
        listeners.call('RESET')
        listeners.call('GET', 'mode-restricted')
        listeners.subscription('SUBSCRIBE', 'all-db')
        listeners.call('RESET')
    finally:
        listeners.close()
        admin.close()
    drain(ports)


def raw_roundtrip(port, commands, protocol):
    client = RespClient(port)
    try:
        if protocol == 3:
            client.call('HELLO', 3)
        encoded = b''.join(Client.encode(args) for args in commands)
        client.socket.sendall(encoded)
        client.socket.settimeout(.1)
        wire = b''
        try:
            while True:
                data = client.socket.recv(65536)
                if not data:
                    break
                wire += data
        except socket.timeout:
            pass
        return wire
    finally:
        client.close()


def pipelines(ports):
    global CHECKS
    cases = [
        [['PING'], ['SUBSCRIBE', 'a', 'b'], ['PING'],
         ['UNSUBSCRIBE', 'a', 'b'], ['PING']],
        [['MULTI'], ['SUBSCRIBE', 'x', 'y'], ['PUBLISH', 'x', 'self'],
         ['PING'], ['EXEC']],
        [['MULTI'], ['SUBSCRIBE', 'x'], ['PUBLISH', 'x', 'self'],
         ['UNSUBSCRIBE', 'x'], ['PING'], ['EXEC']],
        [['MULTI'], ['SUBSCRIBE', 'cancelled'], ['DISCARD'], ['PING']],
    ]
    for protocol in [2, 3]:
        for case in cases:
            values = [raw_roundtrip(port, case, protocol) for port in ports]
            assert values[0] == values[1], (protocol, case, values)
            CHECKS += 1
            drain(ports)


def protocol_in_exec(ports):
    for initial, final in [(2, 3), (3, 2)]:
        clients = Pair(ports, initial)
        try:
            commands = [['MULTI'], ['SUBSCRIBE', 'switch'],
                        ['PUBLISH', 'switch', 'old'], ['HELLO', final],
                        ['PUBLISH', 'switch', 'new'], ['PING'], ['EXEC']]
            for client in clients.clients:
                client.socket.sendall(b''.join(Client.encode(args)
                                              for args in commands))
            for _ in range(6):
                equal(clients.clients[0].read(), clients.clients[1].read(),
                      'protocol EXEC queued replies')
            outer = [client.read() for client in clients.clients]
            assert all(reply.kind == b'*' and len(reply.value) == 5
                       for reply in outer), outer
            for reply in outer:
                hello = reply.value[2]
                assert hello.kind == (b'%' if final == 3 else b'*')
                assert mapping(hello)[b'proto'].value == final
            for position in [0, 1, 3, 4]:
                equal(outer[0].value[position], outer[1].value[position],
                      ('protocol EXEC child', position))
            old = equal(clients.clients[0].read(), clients.clients[1].read(),
                        'old protocol self publication')
            new = equal(clients.clients[0].read(), clients.clients[1].read(),
                        'new protocol self publication')
            assert old.kind == (b'>' if initial == 3 else b'*')
            assert new.kind == (b'>' if final == 3 else b'*')
            clients.call('RESET')
        finally:
            clients.close()
        drain(ports)


def permissions(ports):
    admin, listeners = Pair(ports), Pair(ports, 3)
    try:
        admin.call('ACL', 'SETUSER', 'listener', 'reset', 'on', '>pw',
                   '+@all', 'resetchannels', '&events:*')
        listeners.call('AUTH', 'listener', 'pw')
        listeners.subscription('SUBSCRIBE', 'events:a')
        listeners.subscription('PSUBSCRIBE', 'events:*')
        listeners.subscription('SSUBSCRIBE', 'events:a')
        listeners.call('SUBSCRIBE', 'denied')
        listeners.call('PSUBSCRIBE', 'events:a')
        listeners.call('SPUBLISH', 'denied', 'payload')
        listeners.call('PUBLISH', 'events:a', 'denied-payload')
        # A publishing subscriber sees its integer reply before self pushes.
        listeners.messages(2, unordered=True)
        for args in [('SUBSCRIBE', 'denied'), ('PSUBSCRIBE', 'events:a'),
                     ('PUBLISH', 'events:a', 'denied-payload')]:
            admin.call('ACL', 'DRYRUN', 'listener', *args)
        admin.call('ACL', 'SETUSER', 'listener', '-subscribe')
        assert admin.call('PUBLISH', 'events:a', 'still-subscribed').value == 2
        listeners.messages(2, unordered=True)
        admin.call('ACL', 'SETUSER', 'listener', 'resetchannels', '&other:*')
        for client in listeners.clients:
            assert client.stream.readline() == b''
        drain(ports)
        admin.call('ACL', 'SETUSER', 'selector', 'reset', 'on', '>pw',
                   '+subscribe', '&a*', '(+subscribe &b*)')
        replacement = Pair(ports, 3)
        try:
            replacement.call('AUTH', 'selector', 'pw')
            replacement.call('SUBSCRIBE', 'a1', 'b1')
            for client in admin.clients:
                log = client.call('ACL', 'LOG', 1).value[0].value
                entry = dict((k.value, v) for k, v in zip(log[::2], log[1::2]))
                assert entry[b'reason'].value == b'channel' and \
                    entry[b'object'].value == b'b1', entry
        finally:
            replacement.close()
    finally:
        admin.close()
        listeners.close()
    drain(ports)


def auth_switch_and_self_close(ports):
    admin, listeners = Pair(ports), Pair(ports, 3)
    replacement = None
    try:
        admin.call('ACL', 'SETUSER', 'old-user', 'reset', 'on', '>pw',
                   '+@all', '&old:*')
        admin.call('ACL', 'SETUSER', 'new-user', 'reset', 'on', '>pw',
                   '+@all', '&new:*')
        listeners.call('AUTH', 'old-user', 'pw')
        listeners.subscription('SUBSCRIBE', 'old:a', 'old:b')
        listeners.call('AUTH', 'new-user', 'pw')
        listeners.subscription('UNSUBSCRIBE', 'old:a')
        assert admin.call('PUBLISH', 'old:b', 'after-auth').value == 1
        listeners.messages(1)
        admin.call('ACL', 'SETUSER', 'unrelated', 'reset', 'on', 'nopass',
                   '+@all', '&unrelated:*')
        admin.call('ACL', 'SETUSER', 'unrelated', 'resetchannels')
        assert admin.call('PUBLISH', 'old:b', 'retained').value == 1
        listeners.messages(1)
        admin.call('ACL', 'SETUSER', 'new-user', '+ping')
        for client in listeners.clients:
            assert client.stream.readline() == b''
        replacement = Pair(ports, 3)
        replacement.call('AUTH', 'new-user', 'pw')
        replacement.subscription('SUBSCRIBE', 'new:a')
        assert replacement.call('ACL', 'SETUSER', 'new-user',
                                'resetchannels', '&other:*').value == b'OK'
        for client in replacement.clients:
            assert client.stream.readline() == b''
    finally:
        admin.close()
        listeners.close()
        if replacement:
            replacement.close()
    drain(ports)


def concurrent(ports):
    for protocol in [2, 3]:
        listeners = Pair(ports, protocol)
        try:
            listeners.subscription('SUBSCRIBE', 'parallel')
            def publish(worker):
                clients = Pair(ports)
                try:
                    for position in range(256):
                        value = '%d:%d' % (worker, position)
                        for client in clients.clients:
                            assert client.call('PUBLISH', 'parallel',
                                               value).value == 1
                finally:
                    clients.close()
            with ThreadPoolExecutor(max_workers=4) as pool:
                list(pool.map(publish, range(4)))
            payloads = []
            for client in listeners.clients:
                next_position = [0] * 4
                messages = []
                for _ in range(1024):
                    reply = client.read()
                    assert reply.kind == (b'>' if protocol == 3 else b'*')
                    value = reply.value[2].value
                    worker, position = map(int, value.split(b':'))
                    assert position == next_position[worker], value
                    next_position[worker] += 1
                    messages.append(value)
                assert next_position == [256] * 4
                payloads.append(sorted(messages))
            assert payloads[0] == payloads[1]
            listeners.call('RESET')
        finally:
            listeners.close()
        drain(ports)


def membership_races(ports):
    for port in ports:
        listener = RespClient(port)
        listener.call('HELLO', 3)
        stop, started, ready = (threading.Event() for _ in range(3))
        delivered = 0
        def publish():
            client = RespClient(port)
            try:
                assert ready.wait(3)
                position = 0
                while not stop.is_set() and position < 4096:
                    assert client.call('PUBLISH', 'race', position).value in \
                        (0, 1)
                    position += 1
                    started.set()
            finally:
                client.close()
        try:
            with ThreadPoolExecutor(max_workers=1) as pool:
                worker = pool.submit(publish)
                try:
                    for _ in range(64):
                        ack = listener.call('SUBSCRIBE', 'race')
                        assert ack.value[0].value == b'subscribe', ack
                        ready.set()
                        assert started.wait(3)
                        listener.socket.sendall(Client.encode(
                            ['UNSUBSCRIBE', 'race']))
                        while True:
                            event = listener.read()
                            assert event.kind == b'>'
                            if event.value[0].value == b'unsubscribe':
                                break
                            assert event.value[0].value == b'message', event
                            delivered += 1
                        assert listener.call('PING').value == b'PONG'
                finally:
                    stop.set()
                    ready.set()
                worker.result()
            assert delivered > 0, 'publisher never overlapped membership'
        finally:
            stop.set()
            listener.close()
    drain(ports)


def slow_reader(ports):
    for port in ports:
        listener, publisher = RespClient(port), RespClient(port)
        try:
            listener.socket.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF,
                                       4096)
            listener.call('SUBSCRIBE', 'temporary-slow')
            listener.call('UNSUBSCRIBE', 'temporary-slow')
            listener.call('SUBSCRIBE', 'slow')
            payload = b'x' * (1024 * 1024)
            for _ in range(96):
                publisher.call('PUBLISH', 'slow', payload)
                remaining = publisher.call('PUBSUB', 'NUMSUB', 'slow')
                if remaining.value[1].value == 0:
                    break
            else:
                raise AssertionError('slow subscriber did not hit quota')
            assert publisher.call('PING').value == b'PONG'
        finally:
            listener.close()
            publisher.close()
    drain(ports)


def normal_reply_quota(ports):
    payload = b'x' * (33 * 1024 * 1024 - 1) + b'\0'
    for protocol in [2, 3]:
        listener = Pair(ports, protocol)
        try:
            for reset in [False, True]:
                listener.subscription('SUBSCRIBE', 'quota-mode')
                if reset:
                    listener.call('RESET')
                else:
                    listener.subscription('UNSUBSCRIBE', 'quota-mode')
                for client in listener.clients:
                    reply = client.call('ECHO', payload)
                    assert reply.kind == b'$' and reply.value == payload
                listener.call('PING')
            # Re-entering subscription mode restores the bounded policy.
            listener.subscription('SUBSCRIBE', 'quota-mode')
            listener.call('RESET')
        finally:
            listener.close()
        drain(ports)


def acl_load(binaries):
    with tempfile.TemporaryDirectory(prefix='pkv-pubsub-acl-') as directory:
        paths = [Path(directory) / name for name in ['left.acl', 'right.acl']]
        rules = ('user default on nopass ~* &* +@all\n'
                 'user reload-user on >pw ~* &reload:* +@all\n')
        for path in paths:
            path.write_text(rules)
        with server(binaries[0], aclfile=paths[0]) as left, \
                server(binaries[1], redis=True, aclfile=paths[1]) as right:
            ports = [left, right]
            admin, listener = Pair(ports), Pair(ports, 3)
            try:
                listener.call('AUTH', 'reload-user', 'pw')
                listener.subscription('SUBSCRIBE', 'reload:a')
                for path in paths:
                    path.write_text(rules.replace('&reload:*', '&other:*'))
                admin.call('ACL', 'LOAD')
                for client in listener.clients:
                    assert client.stream.readline() == b''
                drain(ports)
            finally:
                admin.close()
                listener.close()


def cluster(binaries):
    with server(binaries[0], cluster=True) as left, \
            server(binaries[1], redis=True, cluster=True) as right:
        ports = [left, right]
        listener, admin = Pair(ports), Pair(ports)
        try:
            listener.subscription('SUBSCRIBE', 'a')
            assert admin.call('PUBLISH', 'a', 'no-slots').value == 1
            listener.messages(1)
            admin.call('SSUBSCRIBE', 'a', 'b')
            admin.call('SSUBSCRIBE', '{x}a', '{x}b')
            admin.subscription('SUNSUBSCRIBE', 'a', 'b')
            admin.subscription('SUNSUBSCRIBE')
            admin.call('SPUBLISH', 'a', 'v')
            admin.call('MULTI')
            admin.call('SSUBSCRIBE', '{x}a')
            admin.call('EXEC')
            listener.call('RESET')
        finally:
            listener.close()
            admin.close()
        drain(ports)


def replica_policy(binaries):
    with socket.socket() as reserved:
        reserved.bind(('127.0.0.1', 0))
        upstream = reserved.getsockname()[1]
        with server(binaries[0], replica_rpc_source='127.0.0.1:%d' % upstream
                    ) as left, server(binaries[1], redis=True) as right:
            admin = Pair([left, right])
            admin.clients[1].call('REPLICAOF', '127.0.0.1', upstream)
            try:
                for protocol in [2, 3]:
                    listener = Pair([left, right], protocol)
                    try:
                        listener.subscription('SUBSCRIBE', 'replica-events')
                        for command in [('SET', 'key', 'value'),
                                        ('DEL', 'key'), ('LPUSH', 'key', 'v'),
                                        ('BLPOP', 'key', 1)]:
                            assert listener.call(*command).value.startswith(
                                b'READONLY'), command
                        listener.call('GET', 'key')
                        assert admin.call('PUBLISH', 'replica-events',
                                          'local').value == 1
                        listener.messages(1)
                        if protocol == 3:
                            listener.call('MULTI')
                            listener.call('PUBLISH', 'replica-events', 'exec')
                            listener.call('PING')
                            listener.call('EXEC')
                            listener.messages(1)
                            listener.subscription('SSUBSCRIBE', 'replica-shard')
                            assert admin.call('SPUBLISH', 'replica-shard',
                                              'shard-local').value == 1
                            listener.messages(1)
                        listener.call('RESET')
                    finally:
                        listener.close()
                    drain([left, right])
            finally:
                admin.close()

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--pkv', required=True)
    parser.add_argument('--redis', required=True)
    args = parser.parse_args()
    binaries = [str(Path(args.pkv).resolve()), str(Path(args.redis).resolve())]
    with server(binaries[0]) as left, \
            server(binaries[1], redis=True) as right:
        ports = [left, right]
        for exercise in [basic, shard_only, shared_patterns, descriptions,
                         blocked, database_and_protocol,
                         pipelines, protocol_in_exec, permissions,
                         auth_switch_and_self_close,
                         concurrent, membership_races, slow_reader,
                         normal_reply_quota]:
            exercise(ports)
            print('PubSub scenario passed:', exercise.__name__, flush=True)
    acl_load(binaries)
    print('PubSub scenario passed: acl_load', flush=True)
    cluster(binaries)
    print('PubSub scenario passed: cluster', flush=True)
    replica_policy(binaries)
    print('PubSub scenario passed: replica_policy', flush=True)
    print('PubSub typed differential assertions:', CHECKS)
    print('PubSub binary command safety assertions:', SAFE_CHECKS)
    print('PubSub commands, ACL, lifecycle and concurrent delivery passed')


if __name__ == '__main__':
    main()
