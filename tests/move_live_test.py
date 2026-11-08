#!/usr/bin/env python3
"""Actual frontend MOVE publication into a live memory replica."""
import argparse
import time
from integration_test import Client, server
from exec_live_test import Peer
from ordered_live_test import commands, view, wait_value


def ready(source, target):
    deadline = time.monotonic() + 8
    for number in range(1000):
        assert source.call('MULTI') == b'OK'
        assert source.call('SET', 'move:ready-marker', number) == b'QUEUED'
        assert source.call('EXEC') == [b'OK']
        if target.call('GET', 'move:ready-marker') == str(number).encode():
            return
        assert time.monotonic() < deadline
        time.sleep(.02)
    raise AssertionError('MOVE replica did not negotiate group support')


def run(binary):
    with server(binary, active_expire=True) as primary:
        address = '127.0.0.1:%d' % (primary + 10000)
        with server(binary, backend='mdb', replica_rpc_source=address) as port:
            source, target = Client(primary), Client(port)
            try:
                ready(source, target)
                assert target.call('MOVE', 'missing', 7).startswith(b'READONLY')
                for fixture in ('string', 'hash', 'list', 'set', 'zset', 'small-zset'):
                    key = fixture.encode() + b'\0frontend-move'
                    kind = 'zset' if fixture == 'small-zset' else fixture
                    if fixture == 'small-zset':
                        create = ('ZADD', key, 1, b'a\0', 2, 'side')
                        update = ('ZADD', key, 3, 'later')
                    else:
                        create, update = commands(kind, key)
                    assert source.call('SELECT', 0) == b'OK'
                    assert target.call('SELECT', 0) == b'OK'
                    assert source.call(*create) is not None
                    before = view(source, kind, key)
                    wait_value(target, kind, key, before)
                    assert source.call('PEXPIREAT', key, 4102444800123) == 1
                    assert source.call('MOVE', key, 7) == 1
                    assert source.call('SELECT', 7) == b'OK'
                    assert target.call('SELECT', 7) == b'OK'
                    wait_value(target, kind, key, before)
                    assert target.call('PEXPIRETIME', key) == 4102444800123
                    assert source.call('MULTI') == b'OK'
                    assert source.call(*update) == b'QUEUED'
                    assert source.call('MOVE', key, 15) == b'QUEUED'
                    assert source.call('SELECT', 15) == b'QUEUED'
                    assert source.call('MOVE', key, 0) == b'QUEUED'
                    assert source.call('SELECT', 0) == b'QUEUED'
                    replies = source.call('EXEC')
                    assert replies[1:] == [1, b'OK', 1, b'OK'], replies
                    assert target.call('SELECT', 0) == b'OK'
                    wait_value(target, kind, key, view(source, kind, key))
                    for selected in (7, 15):
                        assert target.call('SELECT', selected) == b'OK'
                        assert target.call('TYPE', key) == b'none'
                assert source.call('SELECT', 0) == b'OK'
                expiry_peer = Peer(primary, 4)
                try:
                    key = b'move:expiry-index'
                    assert source.call('SET', key, 'v', 'PX', 1000) == b'OK'
                    expiry_peer.ordinary()
                    assert source.call('MOVE', key, 7) == 1
                    assert [expiry_peer.read()[1] for _ in range(3)] == [
                        14, 16, 15]
                    # No GET/TYPE on the source: observe active expiry itself.
                    depth, expired = 0, False
                    while True:
                        record = expiry_peer.read()
                        if record[1] in (5, 14):
                            depth += 1
                        if record[1] in (11, 15):
                            depth -= 1
                        if record[1] in (3, 8) and record[2:4] == (7, key):
                            expired = True
                        if expired and depth == 0:
                            break
                finally:
                    expiry_peer.close()
                peer = Peer(primary, 3)
                try:
                    assert source.call('SET', 'move:old-peer', 'value') == b'OK'
                    peer.ordinary()
                    assert source.call('MOVE', 'move:old-peer', 7) == 1
                    assert peer.socket.recv(1) == b''
                finally:
                    peer.close()
            finally:
                source.close()
                target.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--pkv', required=True)
    run(parser.parse_args().pkv)
    print('Frontend MOVE source replication, compact ZSets and old-peer refusal')
