#!/usr/bin/env python3
"""Exercise redo against real engines, terminating only owned child PIDs."""
from pathlib import Path
import select
import signal
import subprocess
import sys
import tempfile

helper, backend = sys.argv[1:3]


def check_sanitizers(errors):
    for marker in [b'ERROR: AddressSanitizer', b'runtime error:',
                   b'SUMMARY: AddressSanitizer', b'SUMMARY: UndefinedBehavior',
                   b'ASan is ignoring requested __asan_handle_no_return']:
        assert marker not in errors, errors


def run(mode, root, argument='', expected=0):
    result = subprocess.run([helper, backend, mode, root, argument],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            timeout=60)
    check_sanitizers(result.stderr)
    if expected == -signal.SIGABRT and argument and mode == '--recover':
        assert ('FAULT ' + argument + '\n').encode() in result.stderr, result
    if result.returncode != expected:
        raise AssertionError((mode, argument, result.returncode,
                              result.stdout, result.stderr))
    return result


with tempfile.TemporaryDirectory(prefix='pkv-redo-exercise-') as root:
    print(run('--exercise', root).stdout.decode(), end='')

points = ['source-synced', 'target-cleared', 'page', 'target-published',
          'target-synced', 'source-erased', 'source-erased-synced', 'completed']
for point in points:
    with tempfile.TemporaryDirectory(prefix='pkv-redo-kill-') as root:
        run('--seed', root)
        with open(Path(root) / 'writer.err', 'wb') as errors:
            writer = subprocess.Popen([helper, backend, '--recover', root,
                                       'kill:' + point],
                                      stdout=subprocess.PIPE, stderr=errors)
            try:
                ready, _, _ = select.select([writer.stdout], [], [], 20)
                assert ready, (point, 'cutpoint timed out')
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
        check_sanitizers((Path(root) / 'writer.err').read_bytes())
        run('--recover', root)
        run('--check', root)
        run('--recover', root)
        run('--check', root)
print(backend + ': eight interrupted native redo cutpoints passed')

faults = ['source-read', 'source-stream', 'target-page', 'target-root',
          'target-sync', 'source-erase', 'source-sync', 'journal-remove',
          'journal-sync', 'throw:page', 'throw:target-published',
          'alias', 'memory', 'unopened', 'target-page-after',
          'target-root-after', 'source-erase-after']
for fault in faults:
    with tempfile.TemporaryDirectory(prefix='pkv-redo-fault-') as root:
        run('--seed', root)
        run('--recover', root, fault, -signal.SIGABRT)
        run('--recover', root)
        run('--check', root)
print(backend + ': seventeen failed native redo paths abort and recover passed')

for corruption in ['big-root', 'big-page', 'zero-id', 'range', 'both-missing']:
    with tempfile.TemporaryDirectory(prefix='pkv-redo-corrupt-') as root:
        run('--seed', root, corruption)
        original = (Path(root) / 'decision').read_bytes()
        for _ in range(2):
            run('--recover', root, '', -signal.SIGABRT)
            assert (Path(root) / 'decision').read_bytes() == original
print(backend + ': corrupt native payloads retain the pending decision passed')
