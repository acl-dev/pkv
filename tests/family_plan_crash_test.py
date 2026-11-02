#!/usr/bin/env python3
"""Failure before publication rolls back; failure after publication aborts."""
import signal
import subprocess
import sys


def main():
    binary, engine = sys.argv[1:]
    for phase in ['prepared', 'published', 'completed']:
        result = subprocess.run([binary, engine, phase], capture_output=True,
                                timeout=30)
        expected = 0 if phase == 'prepared' else -signal.SIGABRT
        assert result.returncode == expected, (phase, result.returncode,
                                               result.stderr[-4000:])
    print('Family plan failure boundary rollback/abort passed')


if __name__ == '__main__':
    main()
