#!/usr/bin/env python3
"""Terminate actual EXEC at its source engine commit boundary, then reopen."""
from pathlib import Path
import subprocess
import sys
import tempfile

binary, backend = sys.argv[1:]
with tempfile.TemporaryDirectory(prefix='pkv-source-exec-') as root:
    for phase, expected in [('before', 71), ('after', 72), ('ack', 73)]:
        path = str(Path(root) / phase)
        result = subprocess.run([binary, backend, path, phase])
        assert result.returncode == expected, (phase, result.returncode)
        subprocess.run([binary, backend, path, 'verify-' + phase], check=True)
