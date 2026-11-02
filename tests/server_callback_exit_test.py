#!/usr/bin/env python3
"""ACL idle/use exits must close stores despite retained replica owners."""
import argparse
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time

from integration_test import Client, ports, server
from server_exit_test import values


def ready(process, port):
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        assert process.poll() is None, ('startup exit', process.returncode)
        try:
            client = Client(port)
        except OSError:
            time.sleep(.02)
            continue
        try:
            assert client.call('PING') == b'PONG'
        finally:
            client.close()
        return
    raise AssertionError('startup timeout')


def configuration(root, backend, port, mode, active, source):
    settings = [
        'master_service = 127.0.0.1|%d' % port,
        'master_type = sock', 'master_reuseport = yes',
        'fiber_threads = 2', 'ocache_max = 100', 'db_count = 16',
        'databases = 16', 'dbtype = ' + backend,
        'dbpath = ' + str(root / 'storage'),
        'service = 127.0.0.1:%d' % port,
        'active_expire = %d' % active,
        'fiber_queue_dir = ' + str(root),
    ]
    if mode in ('idle', 'used'):
        settings.append('fiber_idle_limit = 1' if mode == 'idle'
                        else 'fiber_use_limit = 2')
    if source is not None:
        settings += ['replica_rpc_source = 127.0.0.1:%d' % source,
                     'replica_read_only = 0']
    path = root / 'pkv.cf'
    path.write_text('service pkv\n{\n' + '\n'.join(settings) + '\n}\n')
    return path


def clean_stores(checker, storage):
    for home in (storage / 'wdb', storage / 'logical-db-7' / 'wdb'):
        result = subprocess.run([checker, str(home)], capture_output=True,
                                text=True, timeout=10)
        assert result.returncode == 0, (home, result.stdout, result.stderr)


def start(args, config, root, mode, log):
    options = dict(cwd=root, stdout=log, stderr=subprocess.STDOUT)
    if mode != 'supervisor':
        return subprocess.Popen([args.pkv, 'alone', str(config)],
                                **options), None
    master, status = socket.socketpair()
    # ACL's daemon ABI uses fd 5 for its duplex parent status channel.
    wrapper = ('import os,sys; f=int(sys.argv[1]); '
               'os.dup2(f,5,inheritable=True); '
               'os.execv(sys.argv[2],sys.argv[2:])')
    environment = os.environ.copy()
    environment['MASTER_SERVICE'] = 'DAEMON'
    environment['SERVICE_LOG'] = str(root / 'callback.log')
    try:
        with status:
            process = subprocess.Popen(
                [sys.executable, '-c', wrapper, str(status.fileno()),
                 args.pkv, '-f', str(config)], pass_fds=(status.fileno(),),
                stdin=subprocess.DEVNULL, env=environment, **options)
    except BaseException:
        master.close()
        raise
    return process, master


def exit_server(process, master):
    if master is not None:
        master.settimeout(3)
        assert master.recv(8192), 'no daemon startup notification'
        master.close()
    assert process.wait(timeout=10) == 0


def stopped(args, backend, mode, replica, active, root):
    port = ports()
    with socket.socket() as source:
        source.bind(('127.0.0.1', 0))
        source.listen(1)
        source.settimeout(5)
        upstream = source.getsockname()[1] if replica else None
        config = configuration(root, backend, port, mode, active, upstream)
        with (root / 'server.log').open('w+') as log:
            process, master = start(args, config, root, mode, log)
            failed = True
            try:
                ready(process, port)
                client = Client(port)
                try:
                    values(client, True)
                    if replica:
                        peer, _ = source.accept()
                        with peer:
                            peer.settimeout(3)
                            assert peer.recv(4096), 'no replica negotiation'
                            # Keep the receiver waiting with DB/pool owners.
                            if mode != 'supervisor':
                                client.close()
                                client = None
                            exit_server(process, master)
                    else:
                        if mode != 'supervisor':
                            client.close()
                            client = None
                        exit_server(process, master)
                finally:
                    if client is not None:
                        client.close()
                failed = False
            finally:
                if master is not None:
                    master.close()
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=3)
                log.flush()
                log.seek(0)
                detail = log.read()
                callback = root / 'callback.log'
                if callback.exists():
                    detail += callback.read_text()
                if failed:
                    print('Callback failure:', backend, mode, replica, active)
                print(detail if failed or process.returncode else '', end='')
            assert 'Shutdown requested by service exit callback' in detail
            assert 'Storage shutdown completed' in detail
            if mode == 'supervisor':
                assert 'disconnect(' in detail and 'from acl_master' in detail
    storage = root / 'storage'
    if args.clean_check:
        clean_stores(args.clean_check, storage)
    if backend in ('rdb', 'wdb'):
        with server(args.pkv, backend=backend, databases=16,
                    storage_path=storage) as port:
            client = Client(port)
            try:
                values(client, False)
            finally:
                client.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--pkv', required=True)
    parser.add_argument('--clean-check')
    args = parser.parse_args()
    backend = os.environ.get('PKV_TEST_BACKEND', 'mdb')
    for active in (False, True):
        modes = ('idle', 'used')
        if os.name == 'posix':
            modes += ('supervisor',)
        for mode in modes:
            for replica in (False, True):
                with tempfile.TemporaryDirectory(
                        prefix='pkv-callback-') as directory:
                    stopped(args, backend, mode, replica, active,
                            Path(directory))
    print('ACL callback exits, retained replica owners and restart passed',
          backend)


if __name__ == '__main__':
    main()
