#!/usr/bin/env python3
"""Prepublication rollback and postpublication fail-stop in owned processes."""
import signal
import subprocess
import sys

binary, variant = sys.argv[1:3]
backends = ['mdb', 'tbb'] if variant == 'tbb' else ['mdb']
for backend in backends:
    for order in ['first', 'last']:
        for point in ['validated', 'prepared', 'published', 'complete',
                      'completed']:
            result = subprocess.run([binary, backend, order, point, '--throw'],
                                    stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, timeout=30)
            for marker in [b'ERROR: AddressSanitizer', b'runtime error:',
                           b'SUMMARY: AddressSanitizer',
                           b'SUMMARY: UndefinedBehavior']:
                assert marker not in result.stderr, result
            expected = 0 if point in ['validated', 'prepared'] else (
                -signal.SIGABRT)
            assert result.returncode == expected, (backend, order, point,
                                                   result)
            assert ('FAULT ' + point + '\n').encode() in result.stderr, result
print('Memory family exceptions roll back before publish and abort after it')
