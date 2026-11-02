#!/usr/bin/env python3
"""Cancel real blocked startup and interrupt opened/recovering native stores."""
import argparse
import contextlib
import errno
import os
from pathlib import Path
import select
import signal
import subprocess
import sys
import tempfile
import time

from integration_test import Client, ports, server, stop_server
from server_callback_exit_test import configuration, ready
from server_exit_test import values


def sanitized(text):
    for bad in ('ERROR: AddressSanitizer', 'runtime error:',
                'SUMMARY: AddressSanitizer', 'SUMMARY: UndefinedBehavior',
                'ASan is ignoring requested __asan_handle_no_return'):
        assert bad not in text, text


def fifo_writer(process, path):
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        assert process.poll() is None, process.returncode
        try:
            return os.open(path, os.O_WRONLY | os.O_NONBLOCK)
        except OSError as error:
            assert error.errno == errno.ENXIO, error
            time.sleep(.01)
    raise AssertionError('startup did not open FIFO')


@contextlib.contextmanager
def error_stream(backpressure):
    if not backpressure:
        yield subprocess.STDOUT
        return
    reader, writer = os.pipe()
    try:
        os.set_blocking(writer, False)
        while True:
            try:
                os.write(writer, b'x' * 8192)
            except BlockingIOError:
                break
        while True:
            try:
                os.write(writer, b'x')
            except BlockingIOError:
                break
        os.set_blocking(writer, True)
        # Retain both endpoints, without draining, until the child has exited.
        yield writer
    finally:
        os.close(reader)
        os.close(writer)


def blocked_config(args, backend, received, kind, root, backpressure):
    path = configuration(root, backend, ports(), 'startup', True, None)
    fifo = path if kind == 'config' else root / 'access.acl'
    if kind == 'config':
        path.unlink()
    else:
        path.write_text(path.read_text().replace(
            '\n}', '\naclfile = ' + str(fifo) + '\n}'))
    os.mkfifo(fifo)
    with (root / 'server.log').open('w+') as log, \
            error_stream(backpressure) as errors:
        process = subprocess.Popen([args.pkv, 'alone', str(path)], cwd=root,
                                   stdout=log, stderr=errors)
        writer = None
        try:
            writer = fifo_writer(process, fifo)
            if kind == 'config':
                os.write(writer, b'service pkv\n{\n')
            process.send_signal(received)
            assert process.wait(timeout=2) == 0
        finally:
            if writer is not None:
                os.close(writer)
            if process.poll() is None:
                process.kill()
                process.wait(timeout=3)
            log.seek(0)
            detail = log.read()
            sanitized(detail)
        assert not (root / 'storage').exists()


def interrupted(args, backend, mode, root, phase, received,
                backpressure=False):
    with tempfile.TemporaryFile(mode='w+') as log, \
            error_stream(backpressure) as errors:
        process = subprocess.Popen(
            [args.helper, backend, mode, str(root), phase],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=errors if backpressure else log)
        try:
            available, _, _ = select.select([process.stdout], [], [], 15)
            assert available, (mode, phase, 'helper readiness timeout')
            assert process.stdout.readline() == b'READY\n'
            process.send_signal(received)
            expected = (1 if mode in ('throw', 'return') else
                        7 if mode == 'ready' else 0)
            assert process.wait(timeout=2) == expected
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=3)
            process.stdin.close()
            process.stdout.close()
            log.seek(0)
            detail = log.read()
            sanitized(detail)


def native_open(args, backend, received, root):
    storage = root / 'storage'
    with server(args.pkv, backend=backend, databases=16,
                storage_path=storage) as port:
        client = Client(port)
        try:
            values(client, True)
        finally:
            client.close()
    interrupted(args, backend, 'native', storage, '', received)
    with server(args.pkv, backend=backend, databases=16,
                storage_path=storage) as port:
        client = Client(port)
        try:
            values(client, False)
        finally:
            client.close()


@contextlib.contextmanager
def recovered(args, backend, root):
    port = ports()
    path = configuration(root, backend, port, 'startup', True, None)
    path.write_text(path.read_text().replace(
        'dbpath = ' + str(root / 'storage'),
        'dbpath = ' + str(root / 'a') + ';' + str(root / 'b') +
        '\nmove_journal_path = ' + str(root / 'decision')))
    with (root / 'server.log').open('w+') as log:
        process = subprocess.Popen([args.pkv, 'alone', str(path)], cwd=root,
                                   stdout=log, stderr=subprocess.STDOUT)
        try:
            ready(process, port)
            yield port
        finally:
            stop_server(process, log, sys.exc_info()[0] is not None)


def recovery(args, backend, phase, received, root):
    command = [args.seed, backend, '--seed', str(root), '']
    result = subprocess.run(command, capture_output=True, timeout=40)
    assert result.returncode == 0, result.stderr
    sanitized(result.stderr.decode(errors='replace'))
    interrupted(args, backend, 'recover', root, phase, received)
    assert (root / 'decision').exists()
    with recovered(args, backend, root) as port:
        assert not (root / 'decision').exists()
        client = Client(port)
        try:
            assert client.call('GET', b'start\0owner') is None
            assert client.call('GET', 'neighbor') == b'v' * 32768
            assert client.call('SELECT', 1) == b'OK'
            assert client.call('GET', b'start\0owner') == b'v' * 32768
            assert client.call('GET', 'neighbor') == b'v' * 32768
        finally:
            client.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--pkv', required=True)
    parser.add_argument('--helper', required=True)
    parser.add_argument('--seed')
    args = parser.parse_args()
    backend = os.environ.get('PKV_TEST_BACKEND', 'mdb')
    for received in (signal.SIGTERM, signal.SIGINT):
        for backpressure in (False, True):
            for kind in ('config', 'acl'):
                with tempfile.TemporaryDirectory(
                        prefix='pkv-boot-') as directory:
                    blocked_config(args, backend, received, kind,
                                   Path(directory), backpressure)
            for mode in ('waiting', 'ready', 'throw', 'return'):
                interrupted(args, backend, mode, '', '', received,
                            backpressure)
        if backend in ('rdb', 'wdb'):
            assert args.seed
            with tempfile.TemporaryDirectory(prefix='pkv-boot-') as directory:
                native_open(args, backend, received, Path(directory))
            for phase in ('page', 'target', 'erased'):
                with tempfile.TemporaryDirectory(
                        prefix='pkv-boot-redo-') as directory:
                    recovery(args, backend, phase, received, Path(directory))
    print('Startup cancellation, serving handoff and callback failures passed',
          backend)


if __name__ == '__main__':
    main()
