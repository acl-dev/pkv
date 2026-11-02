#!/usr/bin/env python3
"""Independent reopen verifies derived counts against committed roots."""
import subprocess
import sys
import tempfile

binary, backend = sys.argv[1:]
with tempfile.TemporaryDirectory(prefix='pkv-count-') as root:
    subprocess.run([binary, backend, root], check=True)
    if backend in ('rdb', 'wdb'):
        subprocess.run([binary, backend, root, '--reopen'], check=True)
        paths = root + '/cross-a;' + root + '/cross-b'
        subprocess.run([binary, backend, paths, '--cross-store'], check=True)
