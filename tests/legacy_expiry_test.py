#!/usr/bin/env python3
"""Verify legacy TTL migration and independent native disk reopening."""
import subprocess
import sys
import tempfile


def main():
    binary, backend = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix='pkv-legacy-expiry-') as root:
        subprocess.run([binary, backend, root], check=True)
        if backend in ('rdb', 'wdb'):
            subprocess.run([binary, backend, root, '--reopen'], check=True)


if __name__ == '__main__':
    main()
