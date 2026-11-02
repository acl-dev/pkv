#!/usr/bin/env python3
"""Run native key waits against one isolated optional-backend database."""
import subprocess
import sys
import tempfile

binary, backend = sys.argv[1:]
with tempfile.TemporaryDirectory(prefix='pkv-key-wait-') as root:
    subprocess.run([binary, backend, root], check=True)
