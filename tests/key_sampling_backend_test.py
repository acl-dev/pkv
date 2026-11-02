#!/usr/bin/env python3
"""Independent processes check persistent rank indexes and bounded payloads."""
import subprocess
import sys
import tempfile

binary, backend = sys.argv[1:]
with tempfile.TemporaryDirectory(prefix='pkv-sampling-') as root:
    subprocess.run([binary, backend, root], check=True)
    if backend in ('rdb', 'wdb'):
        subprocess.run([binary, backend, root, '--reopen'], check=True)
        paths = root + '/cross-a;' + root + '/cross-b'
        subprocess.run([binary, backend, paths], check=True)
        subprocess.run([binary, backend, paths, '--reopen'], check=True)
