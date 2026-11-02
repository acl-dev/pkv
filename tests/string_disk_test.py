#!/usr/bin/env python3
"""Persistent typed String pages in an isolated native backend directory."""
import os
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix='pkv-string-pages-') as root:
    environment = os.environ.copy()
    environment['UBSAN_OPTIONS'] = 'halt_on_error=1'
    subprocess.run([sys.argv[1], '--disk', sys.argv[2], root],
                   env=environment, check=True, timeout=60)
