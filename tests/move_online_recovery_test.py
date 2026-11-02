#!/usr/bin/env python3
"""Stop an owned frontend with a published MOVE decision and recover it."""
from pathlib import Path
import argparse
import contextlib
import hashlib
import json
import os
import signal
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor
from integration_test import Client, ports, stop_server


@contextlib.contextmanager
def running(binary, backend, root):
    port = ports()
    config = root / ('server-%d.cf' % port)
    config.write_text('service pkv\n{\n'
                      'master_service = 127.0.0.1|%d\nmaster_type = sock\n'
                      'master_reuseport = yes\nfiber_threads = 2\n'
                      'service = 127.0.0.1:%d\ndbtype = %s\ndbpath = %s\n'
                      'db_count = 16\ndatabases = 16\nactive_expire = 0\n}\n'
                      % (port, port, backend, root / 'data'))
    with (root / ('server-%d.log' % port)).open('w+b') as log:
        process = subprocess.Popen([binary, 'alone', str(config)], cwd=root,
                                   stdout=log, stderr=log)
        state = {'process': process, 'expected_kill': False, 'port': port}
        try:
            deadline = time.monotonic() + 15
            while True:
                assert process.poll() is None, 'service exited during startup'
                try:
                    client = Client(port)
                    assert client.call('PING') == b'PONG'
                    client.close()
                    break
                except (OSError, AssertionError):
                    assert time.monotonic() < deadline, 'startup timeout'
                    time.sleep(.02)
            yield state
        finally:
            if state['expected_kill']:
                assert process.wait(timeout=5) == -signal.SIGKILL
            else:
                stop_server(process, log, sys.exc_info()[0] is not None)
            log.flush()
            log.seek(0)
            errors = log.read()
            for marker in [b'ERROR: AddressSanitizer', b'runtime error:',
                           b'SUMMARY: AddressSanitizer',
                           b'SUMMARY: UndefinedBehavior']:
                assert marker not in errors, errors[-8000:]


def interrupt(process, decision):
    if not decision.is_file():
        return None
    os.kill(process.pid, signal.SIGSTOP)
    pid, status = os.waitpid(process.pid, os.WUNTRACED)
    assert pid == process.pid and os.WIFSTOPPED(status)
    if not decision.is_file():
        os.kill(process.pid, signal.SIGCONT)
        return None
    data = decision.read_bytes()
    assert len(data) > 16
    process.kill()
    assert process.wait(timeout=5) == -signal.SIGKILL
    return data


def disconnected(future):
    try:
        reply = future.result(timeout=5)
    except OSError:
        return
    except AssertionError as error:
        assert str(error).startswith('connection closed or invalid frame:'), (
            error)
    else:
        assert reply == 1, reply


def run(binary, backend, output):
    with tempfile.TemporaryDirectory(prefix='pkv-online-move-') as directory:
        root = Path(directory)
        decision = root / 'data/.pkv-move.decision'
        blob = b'journal-public-move\0' * (4 * 1024 * 1024 // 20)
        selected, decision_bytes = None, None
        with running(binary, backend, root) as state:
            process, port = state['process'], state['port']
            admin = Client(port)
            admin.socket.settimeout(30)
            try:
                assert admin.call('SET', 'neighbor', 'unchanged') == b'OK'
                for attempt in range(3):
                    key = b'online\0move:' + str(attempt).encode()
                    assert admin.call('SET', key, blob) == b'OK'
                    mover = Client(port)
                    mover.socket.settimeout(30)
                    try:
                        with ThreadPoolExecutor(max_workers=1) as pool:
                            moving = pool.submit(mover.call, 'MOVE', key, 7)
                            deadline = time.monotonic() + 30
                            while not moving.done():
                                assert time.monotonic() < deadline
                                decision_bytes = interrupt(process, decision)
                                if decision_bytes is not None:
                                    state['expected_kill'] = True
                                    selected = key
                                    break
                                time.sleep(.0005)
                            if selected is not None:
                                disconnected(moving)
                            else:
                                assert moving.result(timeout=30) == 1
                    finally:
                        mover.close()
                    if selected is not None:
                        break
                assert selected is not None, 'no stopped pending decision'
            finally:
                admin.close()
        with running(binary, backend, root) as state:
            client = Client(state['port'])
            client.socket.settimeout(30)
            try:
                assert client.call('GET', 'neighbor') == b'unchanged'
                assert client.call('GET', selected) is None
                assert client.call('SELECT', 7) == b'OK'
                assert client.call('GET', selected) == blob
                assert not decision.exists(), 'journal not completed'
            finally:
                client.close()
        result = {'backend': backend, 'binary': binary,
                  'blob_bytes': len(blob), 'stopped_pending_decision': True,
                  'intentional_exit': -signal.SIGKILL,
                  'restart_verified': True, 'source_absent': True,
                  'target_complete': True, 'neighbor_unchanged': True,
                  'journal_cleared': True,
                  'decision_sha256': hashlib.sha256(decision_bytes).hexdigest()}
        Path(output).write_text(json.dumps(result, indent=2) + '\n')
        print(json.dumps(result), flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--pkv', required=True)
    parser.add_argument('--backend', required=True)
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    run(args.pkv, args.backend, args.output)
