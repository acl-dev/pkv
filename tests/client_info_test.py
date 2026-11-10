#!/usr/bin/env python3
"""CLIENT INFO/LIST rows, live counters, transaction state and RESP3."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import time
import socket
import subprocess
import threading
from pathlib import Path

from integration_test import Client, ErrorReply, server
from resp3_client import RespClient

FIELDS = set(b'id addr laddr fd name age idle flags db sub psub ssub multi '
             b'watch qbuf qbuf-free argv-mem multi-mem rbs rbp obl oll omem '
             b'tot-mem events cmd user redir resp lib-name lib-ver io-thread '
             b'tot-net-in tot-net-out tot-cmds'.split())
STABLE = b'name flags db sub psub ssub multi watch argv-mem multi-mem cmd '
STABLE += b'user redir resp lib-name lib-ver'


def rows(text):
    assert text == b'' or text.endswith(b'\n'), text
    result = []
    for line in text.splitlines():
        fields = dict(part.split(b'=', 1) for part in line.split(b' '))
        assert set(fields) == FIELDS, fields
        assert int(fields[b'id']) > 0 and int(fields[b'fd']) >= 0
        assert int(fields[b'tot-mem']) > 0
        for name in (b'age idle db sub psub ssub watch qbuf qbuf-free '
                     b'argv-mem multi-mem rbs rbp obl oll omem io-thread '
                     b'tot-net-in tot-net-out tot-cmds').split():
            assert int(fields[name]) >= 0, (name, fields[name])
        result.append(fields)
    return result


def own(client):
    result = rows(client.call('CLIENT', 'INFO'))
    assert len(result) == 1
    return result[0]


def observe(admin, identity):
    result = rows(admin.call('CLIENT', 'LIST', 'ID', identity))
    assert len(result) == 1, result
    return result[0]


def differential(port, reference):
    a, b = Client(port), Client(reference)
    checks = 0
    try:
        for command in [(), ('CLIENT', 'SETNAME', 'a=b'),
                        ('CLIENT', 'SETINFO', 'LIB-NAME', 'sdk'),
                        ('CLIENT', 'SETINFO', 'LIB-VER', '1.0'),
                        ('SELECT', 1), ('PING',), ('RESET',)]:
            if command:
                assert a.call(*command) == b.call(*command)
            x, y = own(a), own(b)
            for name in STABLE.split():
                assert x[name] == y[name], (command, name, x[name], y[name])
                checks += 1
            assert x[b'tot-cmds'] == y[b'tot-cmds']
        for command in [('CLIENT', 'INFO', 'extra'),
                        ('CLIENT', 'LIST', 'TYPE'),
                        ('CLIENT', 'LIST', 'TYPE', 'bad'),
                        ('CLIENT', 'LIST', 'ID'),
                        ('CLIENT', 'LIST', 'ID', '01'),
                        ('CLIENT', 'LIST', 'ID', b'1\0x'),
                        ('CLIENT', 'LIST', 'ID', '9223372036854775808'),
                        ('CLIENT', 'LIST', 'ID', '-0'),
                        ('CLIENT', 'LIST', 'TYPE', 'normal', 'ID', '1')]:
            x, y = a.call(*command), b.call(*command)
            assert isinstance(x, ErrorReply) and x == y, (command, x, y)
            checks += 1
        for kind in ['master', 'replica', 'slave', 'pubsub']:
            assert a.call('CLIENT', 'LIST', 'TYPE', kind) == \
                b.call('CLIENT', 'LIST', 'TYPE', kind) == b''
        assert a.call('CLIENT', 'LIST', 'ID', 0, -1) == b''
        return checks
    finally:
        a.close()
        b.close()


def identities(port):
    admin, first, second = Client(port), Client(port), Client(port)
    try:
        one, two = [c.call('CLIENT', 'ID') for c in [first, second]]
        for c, name in [(first, 'first'), (second, 'second')]:
            assert c.call('CLIENT', 'SETNAME', name) == b'OK'
        found = rows(admin.call('CLIENT', 'LIST', 'ID', two, one, two))
        assert [int(x[b'id']) for x in found] == [two, one, two]
        assert [x[b'name'] for x in found] == [b'second', b'first', b'second']
        for c, identity in [(first, one), (second, two)]:
            info = observe(admin, identity)
            assert info[b'addr'] == ('%s:%s' % c.socket.getsockname()).encode()
            assert info[b'laddr'] == ('%s:%s' % c.socket.getpeername()).encode()
        all_ids = {int(x[b'id']) for x in rows(admin.call('CLIENT', 'LIST'))}
        assert {one, two} <= all_ids
        first.call('WATCH', 'info-watch-a', 'info-watch-b')
        assert observe(admin, one)[b'watch'] == b'2'
        second.call('SET', 'info-watch-a', 'changed')
        changed = observe(admin, one)
        assert changed[b'watch'] == b'0' and b'd' in changed[b'flags']
        first.call('UNWATCH')
        assert observe(admin, one)[b'flags'] == b'N'
        first.call('MULTI')
        first.call('PING')
        first.call('CLIENT', 'SETNAME', 'later')
        queued = observe(admin, one)
        assert queued[b'multi'] == b'2' and queued[b'flags'] == b'x'
        assert queued[b'multi-mem'] == b'22'
        assert queued[b'argv-mem'] == b'0'
        assert queued[b'name'] == b'first'
        assert first.call('EXEC') == [b'PONG', b'OK']
        assert observe(admin, one)[b'name'] == b'later'
        assert admin.call('CLIENT', 'KILL', 'ID', two) == 1
        assert admin.call('CLIENT', 'LIST', 'ID', two) == b''
    finally:
        admin.close()
        first.close()
        second.close()


def counters(port):
    c = Client(port)
    try:
        first = own(c)
        assert first[b'tot-cmds'] == b'0'
        assert first[b'tot-net-in'] == str(len(c.encode(['CLIENT', 'INFO']))) \
            .encode()
        second = own(c)
        assert second[b'tot-cmds'] == b'1'
        expected = len(c.encode(['CLIENT', 'INFO'])) * 2
        assert int(second[b'tot-net-in']) == expected
        # Field order and allocation values differ: retain an actual response
        # for the exact completed-write accounting check instead.
        wire = c.call('CLIENT', 'INFO')
        before = rows(wire)[0]
        after = own(c)
        delivered = len(b'$%d\r\n' % len(wire)) + len(wire) + 2
        assert int(after[b'tot-net-out']) - int(before[b'tot-net-out']) == \
            delivered
        base = int(after[b'tot-cmds'])
        assert c.call('MULTI') == b'OK'
        for command in [('PING',), ('CLIENT', 'INFO'), ('CLIENT', 'INFO')]:
            assert c.call(*command) == b'QUEUED'
        response = c.call('EXEC')
        assert response[0] == b'PONG'
        x, y = rows(response[1])[0], rows(response[2])[0]
        assert x[b'cmd'] == y[b'cmd'] == b'exec'
        assert x[b'multi'] == y[b'multi'] == b'3'
        assert x[b'argv-mem'] == x[b'multi-mem'] == b'24'
        assert int(x[b'tot-cmds']) == base + 3
        assert int(y[b'tot-cmds']) == base + 4
        assert int(own(c)[b'tot-cmds']) == base + 6
    finally:
        c.close()


def resp3(port):
    c = RespClient(port)
    try:
        c.call('HELLO', 3, 'SETNAME', 'typed')
        response = c.call('CLIENT', 'INFO')
        assert response.kind == b'=' and response.value.startswith(b'txt:')
        info = rows(response.value[4:])[0]
        assert info[b'resp'] == b'3' and info[b'name'] == b'typed'
        result = c.call('CLIENT', 'LIST', 'ID', info[b'id'])
        assert result.kind == b'=' and result.value.startswith(b'txt:')
        assert rows(result.value[4:])[0][b'id'] == info[b'id']
    finally:
        c.close()


def fragmented(port):
    admin, target = Client(port), Client(port)
    try:
        identity = target.call('CLIENT', 'ID')
        prefix = b'*3\r\n$3\r\nSET\r\n$8\r\nfragment\r\n$1000\r\n'
        target.socket.sendall(prefix + b'x' * 111)
        deadline = time.monotonic() + 3
        while True:
            info = observe(admin, identity)
            if int(info[b'qbuf']) >= 111:
                break
            assert time.monotonic() < deadline
            time.sleep(0.01)
        assert int(info[b'tot-mem']) >= int(info[b'qbuf'])
        target.socket.sendall(b'x' * 889 + b'\r\n')
        assert target.read() == b'OK'
        assert observe(admin, identity)[b'qbuf'] == b'0'
    finally:
        admin.close()
        target.close()


def churn(port):
    done = threading.Event()

    def monitor():
        c = Client(port)
        count = 0
        try:
            while not done.is_set():
                rows(c.call('CLIENT', 'LIST'))
                count += 1
        finally:
            c.close()
        return count

    def worker(index):
        c = Client(port)
        try:
            for step in range(10):
                name = 'client-%d-%d' % (index, step)
                assert c.call('HELLO', 2, 'SETNAME', name)
                assert own(c)[b'name'] == name.encode()
        finally:
            c.close()

    with ThreadPoolExecutor(max_workers=9) as pool:
        observer = pool.submit(monitor)
        try:
            list(pool.map(worker, range(24)))
        finally:
            done.set()
        assert observer.result() > 0


def backpressure(port):
    admin, target = Client(port), Client(port)
    payload = b'x' * (8 * 1024 * 1024)
    try:
        admin.socket.settimeout(20)
        assert admin.call('SET', 'info-large', payload) == b'OK'
        identity = target.call('CLIENT', 'ID')
        target.socket.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 65536)
        target.socket.sendall(target.encode(['GET', 'info-large']))
        deadline = time.monotonic() + 5
        while True:
            info = observe(admin, identity)
            if b'w' in info[b'events'] and int(info[b'obl']) > len(payload):
                break
            assert time.monotonic() < deadline, info
            time.sleep(0.01)
        assert int(info[b'rbs']) >= int(info[b'obl'])
        assert int(info[b'omem']) >= len(payload)
        assert int(info[b'tot-mem']) >= int(info[b'omem'])
        target.socket.settimeout(20)
        assert target.read() == payload
        deadline = time.monotonic() + 3
        while True:
            info = observe(admin, identity)
            if info[b'obl'] == info[b'omem'] == b'0':
                break
            assert time.monotonic() < deadline
        assert b'w' not in info[b'events']
        assert int(info[b'tot-net-out']) >= len(payload)
    finally:
        admin.close()
        target.close()


def adaptive_receive_buffer(port):
    target, admin = Client(port), Client(port)
    try:
        identity = target.call('CLIENT', 'ID')

        def transport_memory():
            info = observe(admin, identity)
            return int(info[b'tot-mem']) - int(info[b'qbuf-free'])

        before = transport_memory()
        values = [b'\0' * 35999 + bytes([i]) for i in range(32)]
        target.socket.sendall(b''.join(
            target.encode(('HSET', 'info-adaptive', 'field', value))
            for value in values))
        assert [target.read() for _ in values] == [1] + [0] * (len(values) - 1)
        deadline = time.monotonic() + 2
        while True:
            grown = transport_memory()
            if abs(grown - before - 63 * 1024) < 1024:
                break
            assert time.monotonic() < deadline, (before, grown)
            time.sleep(.005)
        for _ in range(4):
            assert target.call('PING') == b'PONG'
        shrunk = transport_memory()
        # The initial 1 KiB buffer grows to 64 KiB, then shrinks to 8 KiB.
        assert abs(shrunk - before - 7 * 1024) < 1024, (before, grown, shrunk)
        assert target.call('HGET', 'info-adaptive', 'field') == values[-1]
        assert target.call('DEL', 'info-adaptive') == 1
    finally:
        target.close()
        admin.close()


def adaptive_receive_burst(port):
    target, admin = Client(port), Client(port)
    try:
        identity = target.call('CLIENT', 'ID')
        before = observe(admin, identity)
        key = 'info-adaptive-burst'
        values = [bytes([i]) * (128 * 1024) for i in range(32)]
        wire = b''.join(target.encode(('SET', key, value)) for value in values)
        target.socket.sendall(wire)
        assert [target.read() for _ in values] == [b'OK'] * len(values)

        # Leave the socket idle without polling it through CLIENT LIST.
        time.sleep(2.6)
        idle = observe(admin, identity)
        assert int(idle[b'tot-mem']) <= int(before[b'tot-mem']) + 1024, idle
        assert int(idle[b'tot-net-in']) - int(before[b'tot-net-in']) == len(wire)
        assert admin.call('GET', key) == values[-1]

        payload = b'\x00\xff' * 65536
        prefix = target.encode(('SET', key, payload))
        tail = 32768
        target.socket.sendall(wire + prefix[:-tail])
        assert [target.read() for _ in values] == [b'OK'] * len(values)
        deadline = time.monotonic() + 3
        while True:
            partial = observe(admin, identity)
            if (int(partial[b'qbuf']) >= len(payload) - tail and
                int(partial[b'tot-mem']) - int(partial[b'qbuf']) -
                    int(partial[b'qbuf-free']) <= int(before[b'tot-mem']) + 2048):
                break
            assert time.monotonic() < deadline, partial
            time.sleep(.02)
        target.socket.sendall(prefix[-tail:] + target.encode(('PING',)))
        assert target.read() == b'OK' and target.read() == b'PONG'
        assert admin.call('GET', key) == payload
        assert admin.call('DEL', key) == 1
    finally:
        target.close()
        admin.close()


def cli(reference, port):
    program = Path(reference).with_name('redis-cli')
    result = subprocess.run([str(program), '-3', '-p', str(port),
                             'CLIENT', 'INFO'], capture_output=True,
                            check=True, timeout=10)
    info = rows(result.stdout.rstrip(b'\n') + b'\n')[0]
    assert info[b'resp'] == b'3' and info[b'cmd'] == b'client|info'


def protected(binary, reference):
    for path, redis in [(reference, True), (binary, False)]:
        with server(path, redis=redis, password='info-secret') as port:
            c = Client(port)
            try:
                for command in [('CLIENT', 'INFO'), ('CLIENT', 'LIST')]:
                    assert c.call(*command).startswith(b'NOAUTH')
                assert c.call('AUTH', 'info-secret') == b'OK'
                assert own(c)[b'user'] == b'default'
            finally:
                c.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--pkv', required=True)
    parser.add_argument('--redis', required=True)
    args = parser.parse_args()
    with server(args.redis, redis=True) as ref, server(args.pkv) as port:
        checks = differential(port, ref)
        for target in [ref, port]:
            identities(target)
            counters(target)
            resp3(target)
            cli(args.redis, target)
        fragmented(port)
        adaptive_receive_buffer(port)
        adaptive_receive_burst(port)
        churn(port)
        backpressure(port)
    protected(args.pkv, args.redis)
    print('CLIENT INFO/LIST stable differential assertions:', checks)
    print('CLIENT INFO/LIST rows, counters, WATCH, EXEC and RESP3 passed')


if __name__ == '__main__':
    main()
