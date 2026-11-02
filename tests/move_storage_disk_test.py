#!/usr/bin/env python3
"""Check physical MOVE decisions and owned native process interruptions."""
from pathlib import Path
import select
import signal
import subprocess
import sys
import tempfile

helper, backend = sys.argv[1:3]


def sanitized(errors):
    for marker in [b'ERROR: AddressSanitizer', b'runtime error:',
                   b'SUMMARY: AddressSanitizer', b'SUMMARY: UndefinedBehavior',
                   b'ASan is ignoring requested __asan_handle_no_return']:
        assert marker not in errors, errors


def run(mode, root, argument='', expected=0):
    result = subprocess.run([helper, backend, mode, root, argument],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            timeout=90)
    sanitized(result.stderr)
    assert result.returncode == expected, (mode, argument, result)
    return result


def recover(root, moved):
    for _ in range(2):
        run('--recover', root)
        run('--check', root, 'moved' if moved else 'original')


with tempfile.TemporaryDirectory(prefix='pkv-coord-exercise-') as root:
    print(run('--exercise', root).stdout.decode(), end='')

points = ['storage-validated', 'storage-source-synced', 'storage-decided',
          'apply-page', 'apply-target-published', 'apply-source-erased',
          'effects', 'storage-completed']
for point in points:
    with tempfile.TemporaryDirectory(prefix='pkv-coord-kill-') as root:
        run('--seed', root)
        with open(Path(root) / 'writer.err', 'wb') as errors:
            writer = subprocess.Popen([helper, backend, '--run', root,
                                       'kill:' + point],
                                      stdout=subprocess.PIPE, stderr=errors)
            try:
                ready, _, _ = select.select([writer.stdout], [], [], 30)
                assert ready, (point, 'coordinator cutpoint timed out')
                line = writer.stdout.readline()
                assert line == ('READY ' + point + '\n').encode(), (
                    point, line, (Path(root) / 'writer.err').read_bytes())
                writer.kill()
                writer.wait(timeout=15)
            finally:
                if writer.poll() is None:
                    writer.kill()
                    writer.wait(timeout=15)
                writer.stdout.close()
        sanitized((Path(root) / 'writer.err').read_bytes())
        recover(root, point not in ('storage-validated',
                                   'storage-source-synced'))
print(backend + ': eight interrupted coordinator states passed')

predecision = ['source-read', 'throw:prepare']
fatal = ['decision-write', 'decision-sync', 'decision-rename',
         'decision-dir-sync', 'target-page', 'target-root', 'target-sync',
         'source-erase', 'source-sync', 'journal-remove', 'journal-sync',
         'throw:storage-decided', 'throw:apply-page', 'throw:effects']
for fault in predecision + fatal:
    with tempfile.TemporaryDirectory(prefix='pkv-coord-fault-') as root:
        run('--seed', root)
        result = run('--run', root, fault,
                     0 if fault in predecision else -signal.SIGABRT)
        assert ('FAULT ' + fault + '\n').encode() in result.stderr, result
        pending = (Path(root) / 'decision').exists()
        unmoved = fault in predecision + ['decision-write', 'decision-sync',
                                        'decision-rename']
        if unmoved or fault == 'journal-sync':
            assert not pending, fault
        else:
            assert pending, fault
        recover(root, not unmoved)
print(backend + ': sixteen coordinator failure/recovery paths passed')
