#!/usr/bin/env python3
"""Canonical native roots must not be reparsed as delimiter lists."""
import argparse
from pathlib import Path
import subprocess
import tempfile
from integration_test import Client, server
from ordered_live_test import commands, view


def aliases(root):
    root.mkdir()
    actual = [root / 'space path', root / 'semi;comma,tab\tpath']
    names = []
    for index, path in enumerate(actual):
        path.mkdir()
        name = root / ('alias-%d' % index)
        name.symlink_to(path, target_is_directory=True)
        names.append(name)
    return actual, names


def paths(names):
    return ';'.join(str(name) for name in names)


def upgrade(args, root):
    actual, names = aliases(root)
    expected = {}
    with server(args.pkv, backend=args.backend, storage_path=paths(names),
                move_journal_path='off') as port:
        client = Client(port)
        try:
            for index in (0, 7, 15):
                assert client.call('SELECT', index) == b'OK'
                assert client.call('SET', 'neighbor', index) == b'OK'
            assert client.call('SELECT', 0) == b'OK'
            for kind in ('string', 'hash', 'list', 'set', 'zset'):
                key = kind.encode() + b'\0paths'
                create, _ = commands(kind, key)
                assert client.call(*create) is not None
                expected[kind] = view(client, kind, key)
                assert client.call('PEXPIREAT', key, 4102444800123) == 1
        finally:
            client.close()
    with server(args.pkv, backend=args.backend,
                storage_path=paths(names)) as port:
        client = Client(port)
        try:
            for kind, value in expected.items():
                key = kind.encode() + b'\0paths'
                assert view(client, kind, key) == value, kind
                assert client.call('MOVE', key, 7) == 1
                assert client.call('TYPE', key) == b'none'
                assert client.call('SELECT', 7) == b'OK'
                assert view(client, kind, key) == value, kind
                assert client.call('PEXPIRETIME', key) == 4102444800123
                assert client.call('SELECT', 0) == b'OK'
        finally:
            client.close()
    markers = [(path / '.pkv-move.domain').read_bytes() for path in actual]
    assert markers[0] == markers[1]
    alternate = []
    for index, path in enumerate(actual):
        alias = root / ('other-%d' % index)
        alias.symlink_to(path, target_is_directory=True)
        alternate.append(alias)
    with server(args.pkv, backend=args.backend,
                storage_path=paths(alternate)) as port:
        client = Client(port)
        try:
            for index in (0, 7, 15):
                assert client.call('SELECT', index) == b'OK'
                assert client.call('GET', 'neighbor') == str(index).encode()
            assert client.call('SELECT', 7) == b'OK'
            for kind, value in expected.items():
                key = kind.encode() + b'\0paths'
                assert view(client, kind, key) == value, kind
                assert client.call('PEXPIRETIME', key) == 4102444800123
                assert client.call('MOVE', key, 0) == 1
        finally:
            client.close()
    assert [(p / '.pkv-move.domain').read_bytes() for p in actual] == markers


def explicit(args, root):
    actual, names = aliases(root / 'explicit')
    control = root / 'control with ;comma,'
    control.mkdir()
    journal = control / 'decision'
    for iteration in range(2):
        with server(args.pkv, backend=args.backend,
                    storage_path=paths(names),
                    move_journal_path=journal) as port:
            client = Client(port)
            try:
                if not iteration:
                    assert client.call('SET', 'key', 'value') == b'OK'
                    assert client.call('MOVE', 'key', 7) == 1
                else:
                    assert client.call('GET', 'key') is None
                    assert client.call('SELECT', 7) == b'OK'
                    assert client.call('GET', 'key') == b'value'
            finally:
                client.close()
    assert all((path / '.pkv-move.domain').is_file() for path in actual)


def run(args):
    with tempfile.TemporaryDirectory(prefix='pkv-native-paths-') as directory:
        root = Path(directory)
        unit = root / 'unit'
        unit.mkdir()
        result = subprocess.run([args.helper, args.backend, str(unit)],
                                capture_output=True, text=True, timeout=45)
        assert result.returncode == 0, (result.stdout, result.stderr)
        assert 'runtime error:' not in result.stderr, result.stderr
        print(result.stdout, end='')
        upgrade(args, root / 'upgrade')
        explicit(args, root)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--pkv', required=True)
    parser.add_argument('--helper', required=True)
    parser.add_argument('--backend', required=True)
    run(parser.parse_args())
    print('Native path aliases, auto binding, five types and restart passed')
