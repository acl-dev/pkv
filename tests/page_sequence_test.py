#!/usr/bin/env python3
"""Run ordered replay against an isolated native engine instance."""
import subprocess
import sys
import tempfile


def main():
    binary, backend = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix='pkv-sequence-') as root:
        subprocess.run([binary, backend, root], check=True)
        if backend in ('rdb', 'wdb'):
            subprocess.run([binary, backend, root, '--reopen'], check=True)
            paths = root + '/cross-a;' + root + '/cross-b'
            subprocess.run([binary, backend, paths, '--cross-store'],
                           check=True)


if __name__ == '__main__':
    main()
