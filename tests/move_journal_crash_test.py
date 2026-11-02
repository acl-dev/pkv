#!/usr/bin/env python3
"""Kill only owned journal writers at each native persistence boundary."""
from pathlib import Path
import select
import subprocess
import struct
import zlib
import sys
import tempfile

helper = sys.argv[1]
points = {
    'temporary': 'empty', 'write': 'empty', 'file-sync': 'empty',
    'close': 'empty', 'rename': 'pending', 'decision-sync': 'pending',
    'acknowledged': 'pending', 'unlink': 'empty',
    'completion-sync': 'empty', 'completed': 'empty',
}
def check_record(path):
    raw = Path(path).read_bytes()
    magic, source, target, serial, domain_size, key_size = struct.unpack(
        '>8sIIQII', raw[:32])
    assert (magic, source, target, serial) == (b'PKVMOVE1', 2, 15, 918273)
    assert len(raw) == 36 + domain_size + key_size
    assert raw[32:32 + domain_size] == b'rdb\0/a;/b;logical=16'
    assert raw[32 + domain_size:-4] == b'key\0\xff\r\n'
    assert struct.unpack('>I', raw[-4:])[0] == zlib.crc32(raw[:-4])


for point, expected in points.items():
    with tempfile.TemporaryDirectory(prefix='pkv-journal-kill-') as directory:
        path = str(Path(directory) / 'decision')
        with open(Path(directory) / 'writer.err', 'wb') as errors:
            writer = subprocess.Popen([helper, '--writer', path, point],
                                      stdout=subprocess.PIPE, stderr=errors)
            try:
                ready, _, _ = select.select([writer.stdout], [], [], 15)
                if not ready:
                    raise AssertionError(f'{point}: no cutpoint acknowledgment')
                line = writer.stdout.readline()
                if line != f'READY {point}\n'.encode():
                    detail = (Path(directory) / 'writer.err').read_text()
                    raise AssertionError((point, line, detail))
                if expected == 'pending':
                    check_record(path)
                writer.kill()
                writer.wait(timeout=15)
            finally:
                if writer.poll() is None:
                    writer.kill()
                    writer.wait(timeout=15)
                writer.stdout.close()
        subprocess.run([helper, '--check', path, expected], check=True,
                       timeout=15)
        subprocess.run([helper, '--check', path, 'empty'], check=True,
                       timeout=15)
print('Ten MOVE decision journal process-kill cutpoints passed')
