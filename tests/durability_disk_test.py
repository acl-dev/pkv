#!/usr/bin/env python3
"""Native WAL barriers: isolated reopen and killed-writer checks."""
import os
from pathlib import Path
import select
import subprocess
import sys
import tempfile
import time


def exercise(binary, backend):
    for shards in (1, 2):
        with tempfile.TemporaryDirectory(prefix='pkv-sync-') as root:
            paths = root if shards == 1 else root + '/a;' + root + '/b'
            subprocess.run([binary, backend, '--exercise', paths],
                           check=True, timeout=30)
    with tempfile.TemporaryDirectory(prefix='pkv-sync-partial-') as root:
        Path(root + '/bad').write_text('not a directory')
        subprocess.run([binary, backend, '--partial-open',
                        root + '/good;' + root + '/bad'],
                       check=True, timeout=30)
    print(backend + ': WAL barrier single/multiple store checks passed')


def crash(binary, backend):
    for deletion, repeats in ((False, 6), (True, 2)):
        for iteration in range(repeats):
            with tempfile.TemporaryDirectory(prefix='pkv-sync-crash-') as root:
                paths = root + '/a;' + root + '/b'
                env = os.environ.copy()
                env['UBSAN_OPTIONS'] = 'halt_on_error=1'
                log = Path(root) / 'writer.log'
                with log.open('w') as errors:
                    mode = '--delete-writer' if deletion else '--writer'
                    writer = subprocess.Popen([binary, backend, mode, paths],
                        stdout=subprocess.PIPE, stderr=errors, env=env)
                    try:
                        ready, _, _ = select.select([writer.stdout], [], [], 15)
                        assert ready, 'writer did not acknowledge a WAL barrier'
                        line = writer.stdout.readline().strip()
                        valid = (line == b'deleted' if deletion
                                 else line.isdigit())
                        assert valid, log.read_text(errors='replace')
                        time.sleep(iteration * .002)
                        writer.kill()
                        writer.wait(timeout=10)
                        mode = '--check-deleted' if deletion else '--check'
                        args = [binary, backend, mode, paths]
                        if not deletion:
                            args.append(line.decode())
                        result = subprocess.run(args, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            timeout=15)
                        assert result.returncode == 0, result.stderr.decode(
                            errors='replace') + log.read_text(errors='replace')
                    finally:
                        if writer.poll() is None:
                            writer.kill()
                            writer.wait(timeout=10)
                        writer.stdout.close()
    print(backend + ': eight acknowledged WAL barrier kill/reopen cases passed')


if __name__ == '__main__':
    binary, backend = sys.argv[1:3]
    if '--crash' in sys.argv[3:]:
        crash(binary, backend)
    else:
        exercise(binary, backend)
