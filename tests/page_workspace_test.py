#!/usr/bin/env python3
"""Native workspace commit, then an independent process reopens disk data."""
import subprocess
import sys
import tempfile

binary, backend = sys.argv[1:]
with tempfile.TemporaryDirectory(prefix='pkv-workspace-') as root:
    subprocess.run([binary, backend, root], check=True)
    if backend in ('rdb', 'wdb'):
        subprocess.run([binary, backend, root, '--reopen'], check=True)
        paths = root + '/cross-a;' + root + '/cross-b'
        subprocess.run([binary, backend, paths, '--cross-store'], check=True)
