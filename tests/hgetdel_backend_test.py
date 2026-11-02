#!/usr/bin/env python3
"""Check native storage and independent disk reopening after HGETDEL."""
import subprocess
import sys
import tempfile

binary, backend = sys.argv[1:]
with tempfile.TemporaryDirectory(prefix='pkv-hgetdel-') as root:
    subprocess.run([binary, backend, root], check=True)
    if backend in ('rdb', 'wdb'):
        subprocess.run([binary, backend, root, '--reopen'], check=True)
