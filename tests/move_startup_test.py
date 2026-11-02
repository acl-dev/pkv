#!/usr/bin/env python3
"""Replay committed native decisions before an owned server serves traffic."""
import contextlib
import os
from pathlib import Path
import resource
import socket
import subprocess
import sys
import tempfile
import time
from integration_test import Client, ports, stop_server

binary, helper, backend = sys.argv[1:4]
owner = b'start\0owner'
payload = b'v' * 32768


def sanitized(log):
    for bad in ['ERROR: AddressSanitizer', 'runtime error:',
                'SUMMARY: AddressSanitizer', 'SUMMARY: UndefinedBehavior',
                'ASan is ignoring requested __asan_handle_no_return']:
        assert bad not in log, log


def fixture(mode, root, phase=''):
    result = subprocess.run([helper, backend, mode, str(root), phase],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            timeout=40)
    sanitized(result.stderr.decode(errors='replace'))
    assert result.returncode == 0, (result.stdout, result.stderr)


@contextlib.contextmanager
def server(root, *, enabled=True, count=16, paths=None, rejected=False,
           journal=None):
    port = ports()
    config = root / ('server-%d.cf' % port)
    selected = paths or str(root / 'a') + ';' + str(root / 'b')
    log_path = root / ('server-%d.log' % port)
    config.write_text('service pkv\n{\n'
                      f'master_service = 127.0.0.1|{port}\n'
                      'master_type = sock\nmaster_reuseport = yes\n'
                      'fiber_threads = 2\nocache_max = 32\ndb_count = 4\n'
                      f'dbtype = {backend}\ndbpath = {selected}\n'
                      f'databases = {count}\nservice = 127.0.0.1:{port}\n'
                      'active_expire = 1\nactive_expire_interval = 1\n'
                      'active_expire_batch = 1024\n' +
                      (f'move_journal_path = {journal or root / "decision"}\n'
                       if enabled else 'move_journal_path = off\n') +
                      '}\n')
    with log_path.open('w+b') as log:
        env = os.environ.copy()
        env['UBSAN_OPTIONS'] = 'halt_on_error=1'
        process = subprocess.Popen([binary, 'alone', str(config)],
                                   cwd=root, stdout=log, stderr=log, env=env,
                                   preexec_fn=lambda: resource.setrlimit(
                                       resource.RLIMIT_CORE, (0, 0)))
        try:
            if rejected:
                process.wait(timeout=20)
                assert process.returncode == 1, log_path.read_text()
                with socket.socket() as connection:
                    connection.settimeout(.2)
                    assert connection.connect_ex(('127.0.0.1',
                                                  port + 10000)) != 0
                yield port
            else:
                deadline = time.monotonic() + 30
                while True:
                    assert process.poll() is None, log_path.read_text()
                    try:
                        client = Client(port)
                        try:
                            assert client.call('PING') == b'PONG'
                        finally:
                            client.close()
                        break
                    except (OSError, AssertionError):
                        if time.monotonic() > deadline:
                            raise AssertionError(log_path.read_text())
                        time.sleep(.03)
                yield port
        finally:
            if rejected:
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=3)
            else:
                stop_server(process, log, sys.exc_info()[0] is not None)
    sanitized(log_path.read_text(errors='replace'))


if backend not in ('rdb', 'wdb'):
    with tempfile.TemporaryDirectory(prefix='pkv-volatile-') as directory:
        root = Path(directory)
        with server(root, rejected=True):
            pass
        with server(root, enabled=False) as port:
            client = Client(port)
            try:
                assert client.call('SELECT', 1) == b'OK'
                assert client.call('SET', 'normal', 'value') == b'OK'
                assert client.call('GET', 'normal') == b'value'
            finally:
                client.close()
        assert not (root / 'a').exists() and not (root / 'decision').exists()
    print(backend + ': volatile journal rejected, ordinary startup passed')
    sys.exit(0)


for phase in ['', 'page', 'target', 'erased', 'expired']:
    with tempfile.TemporaryDirectory(prefix='pkv-startup-redo-') as directory:
        root = Path(directory)
        fixture('--seed', root, phase)
        assert (root / 'decision').exists()
        with server(root) as port:
            assert not (root / 'decision').exists()
            client = Client(port)
            try:
                assert client.call('GET', owner) is None
                assert client.call('SELECT', 1) == b'OK'
                assert client.call('GET', owner) == (
                    None if phase == 'expired' else payload)
                assert client.call('GET', 'neighbor') == payload
                assert client.call('SET', 'after', 'ok') == b'OK'
            finally:
                client.close()
        fixture('--check', root, phase)
        with server(root) as port:
            client = Client(port)
            try:
                assert client.call('SELECT', 1) == b'OK'
                assert client.call('GET', 'after') == b'ok'
            finally:
                client.close()
print(backend + ': five pending service startup states recover before PONG')

for failure in ['disabled', 'count', 'order', 'partial-topology', 'corrupt',
                'marker', 'journal-path']:
    with tempfile.TemporaryDirectory(prefix='pkv-startup-reject-') as directory:
        root = Path(directory)
        fixture('--seed', root)
        valid_log = (root / 'decision').read_bytes()
        marker = root / 'a' / '.pkv-move.domain'
        valid_marker = marker.read_bytes()
        if failure == 'marker':
            damaged = bytearray(valid_marker)
            damaged[-1] ^= 64
            marker.write_bytes(damaged)
        if failure == 'corrupt':
            original = bytearray((root / 'decision').read_bytes())
            original[-1] ^= 64
            (root / 'decision').write_bytes(original)
        original = (root / 'decision').read_bytes()
        selected = (str(root / 'b') + ';' + str(root / 'a')
                    if failure == 'order' else
                    str(root / 'a') + ';' + str(root / 'c')
                    if failure == 'partial-topology' else None)
        with server(root, enabled=failure != 'disabled',
                    count=17 if failure == 'count' else 16,
                    paths=selected, rejected=True,
                    journal=root / 'other' if failure == 'journal-path'
                    else None):
            pass
        assert (root / 'decision').read_bytes() == original
        assert not (root / 'c' / backend).exists()
        assert marker.read_bytes() == (damaged if failure == 'marker'
                                       else valid_marker)
        # Restore only our deliberately corrupted fixture control files.
        (root / 'decision').write_bytes(valid_log)
        marker.write_bytes(valid_marker)
        with server(root) as port:
            assert not (root / 'decision').exists()
            client = Client(port)
            try:
                assert client.call('SELECT', 1) == b'OK'
                assert client.call('GET', owner) == payload
            finally:
                client.close()
print(backend + ': seven unsafe configurations reject and safely retry')

for enabled in [False, True]:
    with tempfile.TemporaryDirectory(prefix='pkv-startup-owner-') as directory:
        root = Path(directory)
        with server(root, enabled=enabled) as port:
            with server(root, rejected=True):
                pass
            if not enabled:
                assert not (root / 'a' / '.pkv-move.domain').exists()
                assert not (root / 'b' / '.pkv-move.domain').exists()
            client = Client(port)
            try:
                assert client.call('SET', 'still-owned', 'ok') == b'OK'
                assert client.call('GET', 'still-owned') == b'ok'
            finally:
                client.close()
print(backend + ': competing startup preserves both existing owner modes')
