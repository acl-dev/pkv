#!/usr/bin/env python3
"""Kill a real writer process and verify complete WAL-recovered page epochs."""
import os
import select
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def run(binary):
    for iteration in range(6):
        with tempfile.TemporaryDirectory(prefix='pkv-rocks-crash-') as root:
            log = Path(root) / 'writer.log'
            env = os.environ.copy()
            env['UBSAN_OPTIONS'] = 'halt_on_error=1'
            with log.open('w+') as errors:
                writer = subprocess.Popen([binary, '--writer', root],
                                          stdout=subprocess.PIPE,
                                          stderr=errors, env=env)
                try:
                    ready, _, _ = select.select([writer.stdout], [], [], 15)
                    assert ready, 'writer did not commit an initial epoch'
                    line = writer.stdout.readline()
                    assert line.strip().isdigit(), log.read_text()
                    acknowledged = int(line)
                    time.sleep(iteration * .002)
                    writer.kill()
                    writer.wait(timeout=10)
                    checked = subprocess.run([binary, '--check', root,
                                              str(acknowledged)], env=env,
                                             stdout=subprocess.PIPE,
                                             stderr=subprocess.PIPE,
                                             timeout=15)
                    assert checked.returncode == 0, checked.stderr.decode(
                        errors='replace')
                finally:
                    if writer.poll() is None:
                        writer.kill()
                        writer.wait(timeout=10)
                    writer.stdout.close()
    print('RocksDB: six killed-writer WAL/page/index recovery cases passed')


if __name__ == '__main__':
    run(sys.argv[1])
