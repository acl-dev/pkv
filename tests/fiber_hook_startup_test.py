#!/usr/bin/env python3
"""Widen the native hook lookup window before concurrent schedulers start."""
import argparse
import os
from pathlib import Path
import tempfile
from server_callback_exit_test import stopped


def run(args):
    previous = os.environ.copy()
    hook = str(Path(args.hook).resolve())
    try:
        existing = os.environ.get('DYLD_INSERT_LIBRARIES', '')
        os.environ['DYLD_INSERT_LIBRARIES'] = hook + (
            ':' + existing if existing else '')
        os.environ['DYLD_PRINT_LIBRARIES'] = '1'
        for active in (False, True):
            for replica in (False, True):
                with tempfile.TemporaryDirectory(
                        prefix='pkv-fiber-hooks-') as directory:
                    root = Path(directory)
                    stopped(args, args.backend, 'supervisor', replica,
                            active, root)
                    assert hook in (root / 'server.log').read_text(), (
                        'test interposer did not load', hook)
    finally:
        os.environ.clear()
        os.environ.update(previous)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--pkv', required=True)
    parser.add_argument('--hook', required=True)
    parser.add_argument('--backend', required=True)
    parser.add_argument('--clean-check')
    run(parser.parse_args())
    print('Concurrent scheduler startup with delayed kevent lookup passed')
