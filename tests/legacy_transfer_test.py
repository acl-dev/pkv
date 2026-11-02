#!/usr/bin/env python3
"""Verify legacy bounded transfers and independent native disk reopening."""
import subprocess
import sys
import tempfile


def main():
    binary, backend = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix='pkv-legacy-transfer-') as root:
        subprocess.run([binary, backend, root], check=True)
        if backend in ('rdb', 'wdb'):
            subprocess.run([binary, backend, root, '--reopen'], check=True)


if __name__ == '__main__':
    main()
