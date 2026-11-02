#!/usr/bin/env python3
"""Independent reopening and unsupported cross-store MSETEX rollback."""
import subprocess
import sys
import tempfile

binary, backend = sys.argv[1:]
with tempfile.TemporaryDirectory(prefix='pkv-msetex-') as root:
    subprocess.run([binary, backend, root], check=True)
    if backend in ('rdb', 'wdb'):
        subprocess.run([binary, backend, root, '--reopen'], check=True)
        paths = root + '/cross-a;' + root + '/cross-b'
        subprocess.run([binary, backend, paths, '--cross-store'], check=True)
