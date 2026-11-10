#!/usr/bin/env python3
"""Checked Redis command matrix using isolated servers and official redis-benchmark.

This measures named workload shapes, not complete command/option coverage. All
fixtures survive each scenario logically unchanged (including conditional and
no-op writes). No connection to an existing Redis/PKV service is accepted.
"""
import argparse
import contextlib
import csv
import ctypes
from dataclasses import dataclass
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import resource
import statistics
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
from integration_test import Client, ErrorReply, ports, stop_server

DEADLINE = 4102444800123
BITMAP = b'\xaa\x55' * 4096
STRING = b's' * 19


def field(i): return 'field:%013d' % i

def member(i): return 'member:%012d' % i

def list_value(i): return 'value:%013d' % i

def key(kind, i): return 'matrix:%s:%012d' % (kind, i)


@dataclass(frozen=True)
class Scenario:
    name: str
    fixture: str
    argv: tuple
    check: str
    expected: object
    bulk: bool = False

    def wire(self, i=None):
        replacement = '__rand_int__' if i is None else '%012d' % i
        return tuple(str(x).replace('__rand_int__', replacement) for x in self.argv)


def scenarios():
    out = []
    def add(name, fixture, args, expected, check='equal', bulk=False):
        out.append(Scenario(name, fixture, tuple(args), check, expected, bulk))
    s = 'matrix:string:__rand_int__'
    add('get.19', 'string', ['GET', s], STRING.decode())
    add('strlen.19', 'string', ['STRLEN', s], 19)
    add('getrange.19', 'string', ['GETRANGE', s, 0, 7], 's' * 8)
    add('mget.8', 'string', ['MGET', s] + [key('string', i) for i in range(7)], ['s' * 19] * 8)
    add('lcs.len.19', 'string', ['LCS', s, key('string', 0), 'LEN'], 19)
    add('getex.read.19', 'string', ['GETEX', s], STRING.decode())
    add('setnx.existing.19', 'string', ['SETNX', s, STRING.decode()], 0)
    add('getset.same.19', 'string', ['GETSET', s, STRING.decode()], STRING.decode())
    add('append.empty.19', 'string', ['APPEND', s, ''], 19)
    large = 'matrix:large:__rand_int__'
    add('get.8192', 'large', ['GET', large], 'a' * 8192, bulk=True)
    add('strlen.8192', 'large', ['STRLEN', large], 8192)
    add('getrange.8192', 'large', ['GETRANGE', large, 4096, 4159], 'a' * 64)
    for cmd in ['INCRBY', 'DECRBY', 'INCRBYFLOAT']:
        add(cmd.lower() + '.zero', 'number', [cmd, 'matrix:number:__rand_int__', 0], '42' if cmd == 'INCRBYFLOAT' else 42)
    bits = 'matrix:bitmap:__rand_int__'
    add('getbit.8192', 'bitmap', ['GETBIT', bits, 0], 1)
    add('bitcount.8192', 'bitmap', ['BITCOUNT', bits], 32768)
    add('bitcount.1024', 'bitmap', ['BITCOUNT', bits, 0, 1023], 4096)
    add('bitcount.128bits', 'bitmap', ['BITCOUNT', bits, 0, 127, 'BIT'], 64)
    add('bitpos.zero.8192', 'bitmap', ['BITPOS', bits, 0], 1)
    add('bitfield_ro.2get', 'bitmap', ['BITFIELD_RO', bits, 'GET', 'u8', 0, 'GET', 'i8', 8], [170, 85])
    add('bitfield.get', 'bitmap', ['BITFIELD', bits, 'GET', 'u8', 0], [170])
    for size in [16, 129, 512]:
        fixture = 'hash%d' % size
        h = 'matrix:%s:__rand_int__' % fixture
        add('hget.%d' % size, fixture, ['HGET', h, field(7)], 'val')
        add('hlen.%d' % size, fixture, ['HLEN', h], size)
        add('hgetall.%d' % size, fixture, ['HGETALL', h], size, 'hash', True)
        if size == 129:
            add('hmget.4', fixture, ['HMGET', h, field(0), field(7), field(128), 'missing'], ['val', 'val', 'val', None])
            add('hstrlen.129', fixture, ['HSTRLEN', h, field(7)], 3)
            add('hexists.129', fixture, ['HEXISTS', h, field(7)], 1)
            add('hkeys.129', fixture, ['HKEYS', h], [field(i) for i in range(size)], 'unordered', True)
            add('hvals.129', fixture, ['HVALS', h], ['val'] * size, 'unordered', True)
            add('hrandfield.4', fixture, ['HRANDFIELD', h, 4], size, 'hash_sample')
            add('hrandfield.4.values', fixture, ['HRANDFIELD', h, 4, 'WITHVALUES'], size, 'hash_pair_sample')
            add('hscan.all.129', fixture, ['HSCAN', h, 0, 'COUNT', 1024], size, 'hash_scan', True)
            add('hsetnx.existing.129', fixture, ['HSETNX', h, field(7), 'val'], 0)
    for size in [16, 129, 512]:
        fixture = 'set%d' % size
        s = 'matrix:%s:__rand_int__' % fixture
        add('scard.%d' % size, fixture, ['SCARD', s], size)
        add('smembers.%d' % size, fixture, ['SMEMBERS', s], [member(i) for i in range(size)], 'unordered', True)
        if size == 129:
            add('sismember.129', fixture, ['SISMEMBER', s, member(7)], 1)
            add('smismember.3', fixture, ['SMISMEMBER', s, member(0), member(128), 'missing'], [1, 1, 0])
            add('srandmember.4', fixture, ['SRANDMEMBER', s, 4], size, 'set_sample')
            add('sscan.all.129', fixture, ['SSCAN', s, 0, 'COUNT', 1024], size, 'set_scan', True)
            add('sadd.existing.129', fixture, ['SADD', s, member(7)], 0)
    for size in [16, 129, 512]:
        fixture = 'zset%d' % size
        z = 'matrix:%s:__rand_int__' % fixture
        add('zcard.%d' % size, fixture, ['ZCARD', z], size)
        add('zscore.%d' % size, fixture, ['ZSCORE', z, member(7)], '7')
        add('zrange.8.scores.%d' % size, fixture, ['ZRANGE', z, 0, 7, 'WITHSCORES'], [x for i in range(8) for x in (member(i), str(i))])
        if size == 129:
            add('zmscore.3', fixture, ['ZMSCORE', z, member(0), member(128), 'missing'], ['0', '128', None])
            add('zrank.129', fixture, ['ZRANK', z, member(7)], 7)
            add('zrevrank.129', fixture, ['ZREVRANK', z, member(7)], size - 8)
            add('zcount.all.129', fixture, ['ZCOUNT', z, '-inf', '+inf'], size)
            add('zcount.4', fixture, ['ZCOUNT', z, 4, 7], 4)
            add('zrange.8', fixture, ['ZRANGE', z, 0, 7], [member(i) for i in range(8)])
            add('zrevrange.8', fixture, ['ZREVRANGE', z, 0, 7], [member(i) for i in range(size - 1, size - 9, -1)])
            add('zrangebyscore.4', fixture, ['ZRANGEBYSCORE', z, 4, 7], [member(i) for i in range(4, 8)])
            add('zrevrangebyscore.4', fixture, ['ZREVRANGEBYSCORE', z, 7, 4], [member(i) for i in range(7, 3, -1)])
            add('zrandmember.4', fixture, ['ZRANDMEMBER', z, 4], size, 'set_sample')
            add('zscan.all.129', fixture, ['ZSCAN', z, 0, 'COUNT', 1024], size, 'zset_scan', True)
            add('zadd.same.129', fixture, ['ZADD', z, 'XX', 'CH', 7, member(7)], 0)
    z = 'matrix:zlex:__rand_int__'
    add('zlexcount.5', 'zlex', ['ZLEXCOUNT', z, '[' + member(3), '[' + member(7)], 5)
    add('zrangebylex.5', 'zlex', ['ZRANGEBYLEX', z, '[' + member(3), '[' + member(7)], [member(i) for i in range(3, 8)])
    add('zrevrangebylex.5', 'zlex', ['ZREVRANGEBYLEX', z, '[' + member(7), '[' + member(3)], [member(i) for i in range(7, 2, -1)])
    l = 'matrix:list512:__rand_int__'
    add('llen.512', 'list512', ['LLEN', l], 512)
    add('lindex.512', 'list512', ['LINDEX', l, 7], list_value(7))
    add('lpos.middle.512', 'list512', ['LPOS', l, list_value(256)], 256)
    add('lrange.8', 'list512', ['LRANGE', l, 0, 7], [list_value(i) for i in range(8)])
    add('lrange.512', 'list512', ['LRANGE', l, 0, -1], [list_value(i) for i in range(512)], bulk=True)
    add('ltrim.all.512', 'list512', ['LTRIM', l, 0, -1], 'OK')
    add('lset.same.512', 'list512', ['LSET', l, 0, list_value(0)], 'OK')
    add('lrem.missing.512', 'list512', ['LREM', l, 0, 'not-present'], 0)
    add('linsert.missing.512', 'list512', ['LINSERT', l, 'BEFORE', 'not-present', 'val'], -1)
    for size in [128, 8192]:
        fixture = 'hll%d' % size
        add('pfcount.%d' % size, fixture, ['PFCOUNT', 'matrix:%s:__rand_int__' % fixture], size, 'hll')
    x = 'matrix:stream128:__rand_int__'
    add('xlen.128', 'stream128', ['XLEN', x], 128)
    add('xrange.8', 'stream128', ['XRANGE', x, '-', '+', 'COUNT', 8], [[str(i) + '-0', ['field', 'val']] for i in range(1, 9)])
    add('xrevrange.8', 'stream128', ['XREVRANGE', x, '+', '-', 'COUNT', 8], [[str(i) + '-0', ['field', 'val']] for i in range(128, 120, -1)])
    add('xrange.128', 'stream128', ['XRANGE', x, '-', '+'], [[str(i) + '-0', ['field', 'val']] for i in range(1, 129)], bulk=True)
    t = 'matrix:ttl:__rand_int__'
    add('ttl.future', 'ttl', ['TTL', t], DEADLINE, 'ttl')
    add('pttl.future', 'ttl', ['PTTL', t], DEADLINE, 'pttl')
    add('expiretime.future', 'ttl', ['EXPIRETIME', t], DEADLINE // 1000)
    add('pexpiretime.future', 'ttl', ['PEXPIRETIME', t], DEADLINE)
    add('type.string', 'ttl', ['TYPE', t], 'string')
    add('exists.4', 'ttl', ['EXISTS', t] + [key('ttl', i) for i in range(3)], 4)
    add('dbsize', '', ['DBSIZE'], 0, 'dbsize')
    add('time', '', ['TIME'], 0, 'time')
    add('echo.19', '', ['ECHO', 'e' * 19], 'e' * 19)
    add('info.memory', '', ['INFO', 'MEMORY'], 0, 'info', True)
    return out


def encoded(value):
    if isinstance(value, str): return value.encode()
    if isinstance(value, list): return [encoded(x) for x in value]
    return value


def checked(case, value, total_keys):
    assert not isinstance(value, ErrorReply), (case.name, value)
    expected = encoded(case.expected)
    mode = case.check
    if mode == 'equal':
        ok = type(value) is type(expected) and value == expected
    elif mode == 'unordered':
        ok = isinstance(value, list) and sorted(value) == sorted(expected)
    elif mode in ('hash', 'hash_scan', 'zset_scan'):
        if mode != 'hash':
            assert isinstance(value, list) and len(value) == 2 and value[0] == b'0', (case.name, value)
            value = value[1]
        expected_map = {(field(i) if mode != 'zset_scan' else member(i)).encode(): (b'val' if mode != 'zset_scan' else str(i).encode()) for i in range(expected)}
        ok = isinstance(value, list) and len(value) == 2 * expected and dict(zip(value[::2], value[1::2])) == expected_map
    elif mode == 'set_scan':
        ok = isinstance(value, list) and len(value) == 2 and value[0] == b'0' and sorted(value[1]) == [member(i).encode() for i in range(expected)]
    elif mode in ('set_sample', 'hash_sample', 'hash_pair_sample'):
        pairs = mode == 'hash_pair_sample'
        values = value[::2] if pairs else value
        universe = {(field(i) if mode.startswith('hash') else member(i)).encode() for i in range(expected)}
        ok = isinstance(value, list) and len(value) == (8 if pairs else 4) and len(set(values)) == 4 and set(values) <= universe and (not pairs or value[1::2] == [b'val'] * 4)
    elif mode == 'hll':
        ok = type(value) is int and expected * .9 <= value <= expected * 1.1
    elif mode in ('ttl', 'pttl'):
        target = expected - int(time.time() * 1000)
        if mode == 'ttl': target //= 1000
        ok = type(value) is int and abs(value - target) <= (2 if mode == 'ttl' else 2000)
    elif mode == 'dbsize':
        ok = value == total_keys
    elif mode == 'time':
        ok = isinstance(value, list) and len(value) == 2 and abs(int(value[0]) - time.time()) < 3 and 0 <= int(value[1]) < 1000000
    elif mode == 'info':
        ok = isinstance(value, bytes) and b'used_memory:' in value and int(next(x.split(b':')[1] for x in value.splitlines() if x.startswith(b'used_memory:'))) > 0
    else: raise AssertionError(mode)
    assert ok, (case.name, 'unexpected reply', str(value)[:800], 'expected', str(expected)[:300])


def exchange(client, commands):
    for start in range(0, len(commands), 64):
        part = commands[start:start + 64]
        client.socket.sendall(b''.join(Client.encode(command) for command, expected in part))
        for command, expected in part:
            reply = client.read()
            assert type(reply) is type(expected) and reply == expected, (command[:3], reply, expected)


def seed(client, fixtures, keys):
    for fixture in fixtures:
        size = int(re.search(r'\d+$', fixture).group()) if re.search(r'\d+$', fixture) else 0
        for i in range(keys):
            k = key(fixture, i)
            if fixture in ('string', 'large', 'number', 'bitmap', 'ttl'):
                value = {'string': STRING, 'large': b'a' * 8192, 'number': b'42', 'bitmap': BITMAP, 'ttl': STRING}[fixture]
                assert client.call('SET', k, value) == b'OK'
                if fixture == 'ttl': assert client.call('PEXPIREAT', k, DEADLINE) == 1
            elif fixture.startswith('hash'):
                assert client.call('HSET', k, *[x for j in range(size) for x in (field(j), 'val')]) == size
            elif fixture.startswith('set'):
                assert client.call('SADD', k, *[member(j) for j in range(size)]) == size
            elif fixture.startswith('zset') or fixture == 'zlex':
                count = size or 129
                assert client.call('ZADD', k, *[x for j in range(count) for x in (j if size else 0, member(j))]) == count
            elif fixture == 'list512':
                assert client.call('RPUSH', k, *[list_value(j) for j in range(512)]) == 512
            elif fixture.startswith('hll'):
                assert client.call('PFADD', k, *['hll:%012d' % j for j in range(size)]) == 1
            elif fixture == 'stream128':
                exchange(client, [(('XADD', k, str(j) + '-0', 'field', 'val'), (str(j) + '-0').encode()) for j in range(1, 129)])
            else: raise AssertionError(fixture)
    assert client.call('DBSIZE') == len(fixtures) * keys


def verify_bank(client, fixtures, keys):
    hll_counts = {}
    for fixture in fixtures:
        size = int(re.search(r'\d+$', fixture).group()) if re.search(r'\d+$', fixture) else 0
        for i in range(keys):
            k = key(fixture, i)
            if fixture in ('string', 'large', 'number', 'bitmap', 'ttl'):
                expected = {'string': STRING, 'large': b'a' * 8192, 'number': b'42', 'bitmap': BITMAP, 'ttl': STRING}[fixture]
                assert client.call('GET', k) == expected, k
                assert client.call('PEXPIRETIME', k) == (DEADLINE if fixture == 'ttl' else -1), k
            elif fixture.startswith('hash'):
                checked(Scenario('verify.' + k, fixture, (), 'hash', size), client.call('HGETALL', k), 0)
            elif fixture.startswith('set'):
                assert sorted(client.call('SMEMBERS', k)) == [member(j).encode() for j in range(size)], k
            elif fixture.startswith('zset') or fixture == 'zlex':
                count = size or 129
                assert client.call('ZRANGE', k, 0, -1, 'WITHSCORES') == [x for j in range(count) for x in (member(j).encode(), str(j if size else 0).encode())], k
            elif fixture == 'list512':
                assert client.call('LRANGE', k, 0, -1) == [list_value(j).encode() for j in range(512)], k
            elif fixture.startswith('hll'):
                count = client.call('PFCOUNT', k)
                assert size * .9 <= count <= size * 1.1
                hll_counts.setdefault(fixture, []).append(count)
            elif fixture == 'stream128':
                assert client.call('XRANGE', k, '-', '+') == [[(str(j) + '-0').encode(), [b'field', b'val']] for j in range(1, 129)], k
    return hll_counts


def digest(path): return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def save_json(path, value):
    temporary = path.with_name(path.name + '.tmp')
    temporary.write_text(json.dumps(value, indent=2))
    temporary.replace(path)


def input_manifest(binaries):
    files = {Path(__file__).resolve(), ROOT / 'tests/integration_test.py', ROOT / 'mdb.cf', ROOT / 'CMakeLists.txt', ROOT / 'Makefile'}
    for folder in ['common', 'coder', 'action', 'dao', 'db', 'cluster', 'gson', 'slave', 'sync', 'rpc', 'test', 'third_party']:
        files.update(p for p in (ROOT / folder).rglob('*') if p.is_file() and p.suffix in ('.h', '.cpp', '.c', '.inc'))
    files.update(ROOT.glob('*.h')); files.update(ROOT.glob('*.cpp'))
    files.update(Path(p).resolve() for p in binaries)
    return {str(p): digest(p) for p in sorted(files)}


def guard(manifest):
    for path, expected in manifest.items():
        assert digest(path) == expected, 'input changed: ' + path


@contextlib.contextmanager
def running(binary, backend, output, label):
    with tempfile.TemporaryDirectory(prefix='pkv-command-matrix-') as directory:
        root = Path(directory); port = ports()
        if backend == 'redis':
            argv = [str(binary), '--bind', '127.0.0.1', '--port', str(port), '--save', '', '--appendonly', 'no', '--dir', str(root)]
            config = None
        else:
            text = (ROOT / 'mdb.cf').read_text().replace('19001', str(port))
            text, replaced = re.subn(r'(?m)^(\s*)fiber_threads\s*=\s*\d+', r'\1fiber_threads = 1', text)
            assert replaced == 1, 'expected one active fiber_threads setting'
            text = text.replace('fiber_queue_dir = {install_path}/var', 'fiber_queue_dir = ' + str(root))
            text = text.replace('dbtype = mdb', 'dbtype = %s\n service = 127.0.0.1:%d' % (backend, port))
            text += '\nocache_max = 10000\n'
            config = root / 'pkv.cf'; config.write_text(text); (output / (label + '.cf')).write_text(text)
            argv = [str(binary), 'alone', str(config)]
        env = dict(os.environ, MASTER_SERVICE='ALONE')
        with (output / (label + '.server.log')).open('w+') as log:
            process = subprocess.Popen(argv, cwd=root, env=env, stdout=log, stderr=subprocess.STDOUT)
            try:
                deadline = time.monotonic() + 15
                while True:
                    assert process.poll() is None, 'server exited during startup'
                    try:
                        c = Client(port)
                        try: assert c.call('PING') == b'PONG'
                        finally: c.close()
                        break
                    except (OSError, AssertionError):
                        assert time.monotonic() < deadline, 'startup timeout'
                        time.sleep(.03)
                yield port, process
            finally:
                stop_server(process, log, sys.exc_info()[0] is not None)


def call_count(client, command):
    command = command.lower().encode()
    result = 0
    for line in client.call('INFO', 'commandstats').splitlines():
        if not line.startswith(b'cmdstat_') or b':' not in line: continue
        name, fields = line[8:].split(b':', 1)
        if name != command and not name.startswith(command + b'|'): continue
        parts = dict(part.split(b'=', 1) for part in fields.split(b',') if b'=' in part)
        result += int(parts[b'calls'])
    return result


class Usage(ctypes.Structure):
    _fields_ = [('uuid', ctypes.c_ubyte * 16), ('before', ctypes.c_uint64 * 6), ('rss', ctypes.c_uint64), ('footprint', ctypes.c_uint64), ('after', ctypes.c_uint64 * 10)]


def cpu(pid):
    if sys.platform == 'darwin':
        library = ctypes.CDLL('/usr/lib/libproc.dylib', use_errno=True)
        result = Usage()
        assert library.proc_pid_rusage(pid, 2, ctypes.byref(result)) == 0
        return int(result.before[0]) + int(result.before[1])
    if sys.platform.startswith('linux'):
        fields = Path('/proc/%d/stat' % pid).read_text().rsplit(')', 1)[1].split()
        return int(fields[11]) + int(fields[12])
    return None


def summarize(rows):
    grouped = {}
    for row in rows:
        grouped.setdefault((row['scenario'], row['depth'], row['profile']), []).append(row)
    result = []
    for scenario, depth in sorted({(x['scenario'], x['depth']) for x in rows}):
        variants = {profile: {'rps': statistics.median(float(x['csv']['rps']) for x in values), 'p99_ms': statistics.median(float(x['csv']['p99_latency_ms']) for x in values), 'rounds': len(values)} for (name, d, profile), values in grouped.items() if name == scenario and d == depth}
        reference = variants.get('redis', {}).get('rps')
        if reference:
            for value in variants.values(): value['redis_rps_ratio'] = value['rps'] / reference
        result.append({'scenario': scenario, 'depth': depth, 'variants': variants})
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pkv', type=Path, required=True)
    parser.add_argument('--tbb-pkv', type=Path)
    parser.add_argument('--redis-server', type=Path, required=True)
    parser.add_argument('--redis-benchmark', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--keys', type=int, default=128)
    parser.add_argument('--requests', type=int, default=98304)
    parser.add_argument('--bulk-requests', type=int, default=8192)
    parser.add_argument('--rounds', type=int, default=3)
    parser.add_argument('--clients', type=int, default=50)
    parser.add_argument('--connections-per-second', type=float, default=100,
                        help='Pace new client connections outside measured regions')
    parser.add_argument('--depths', type=int, nargs='+', default=[1, 64])
    parser.add_argument('--only', nargs='*', help='Exact named scenarios; defaults to all')
    parser.add_argument('--validate-only', action='store_true')
    parser.add_argument('--extra-input', type=Path, action='append', default=[],
                        help='Additional immutable inputs, e.g. oneTBB dylib/build flags')
    args = parser.parse_args()
    assert len(args.depths) == len(set(args.depths)), 'duplicate pipeline depth'
    assert args.keys >= 8 and args.rounds >= 1 and args.clients > 0 and args.connections_per_second > 0 and args.requests > 0 and args.bulk_requests > 0 and all(x > 0 for x in args.depths)
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    profiles = [('mdb', args.pkv.resolve(), 'mdb')]
    if args.tbb_pkv: profiles.append(('tbb', args.tbb_pkv.resolve(), 'mdb_tbb'))
    profiles.append(('redis', args.redis_server.resolve(), 'redis'))
    benchmark = args.redis_benchmark.resolve()
    selected = scenarios()
    if args.only:
        assert set(args.only) <= {x.name for x in selected}, 'unknown scenario'
        selected = [x for x in selected if x.name in args.only]
    names = [x.name for x in selected]
    assert len(set(names)) == len(names) and names
    registered = set(re.findall(r'\badd\("([A-Z][A-Z0-9_]*)"', (ROOT / 'action/redis_service.cpp').read_text()))
    assert {x.argv[0] for x in selected} <= registered
    output = args.output.resolve(); output.mkdir(parents=True, exist_ok=False)
    manifest = input_manifest([p[1] for p in profiles] + [benchmark] + args.extra_input)
    (output / 'inputs.json').write_text(json.dumps(manifest, indent=2))
    metadata = {'argv': sys.argv, 'platform': platform.platform(), 'profiles': [{'name': n, 'binary': str(b), 'backend': kind, 'workers': 1} for n, b, kind in profiles], 'scenarios': [vars(x) for x in selected], 'registered_commands': len(registered), 'covered_commands': sorted({x.argv[0] for x in selected}), 'validate_only': args.validate_only, 'clients': args.clients, 'new_connections_per_second_limit': args.connections_per_second}
    (output / 'manifest.json').write_text(json.dumps(metadata, indent=2))
    rows = []; checks = []; fixtures = sorted({x.fixture for x in selected if x.fixture}); total = len(fixtures) * args.keys
    hll_reference = None
    next_launch = 0.0
    for rep in range(args.rounds):
        order = profiles[rep % len(profiles):] + profiles[:rep % len(profiles)]
        for profile, binary, backend in order:
            label = 'round%d-%s' % (rep, profile)
            guard(manifest)
            with running(binary, backend, output, label) as (port, process):
                c = Client(port); c.socket.settimeout(60)
                try:
                    seed(c, fixtures, args.keys)
                    hll = verify_bank(c, fixtures, args.keys)
                    if hll_reference is None: hll_reference = hll
                    else: assert hll == hll_reference, 'HLL cardinalities differ across profiles'
                    for case in selected:
                        for i in sorted({0, min(7, args.keys - 1), args.keys - 1}): checked(case, c.call(*case.wire(i)), total)
                        checks.append({'round': rep, 'profile': profile, 'scenario': case.name, 'stage': 'preflight'})
                    save_json(output / 'checks.json', checks)
                    if not args.validate_only:
                        depths = args.depths if rep % 2 == 0 else list(reversed(args.depths))
                        for depth in depths:
                            cases = selected if rep % 2 == 0 else list(reversed(selected))
                            for case in cases:
                                n = args.bulk_requests if case.bulk else args.requests
                                argv = [str(benchmark), '-h', '127.0.0.1', '-p', str(port), '-c', str(args.clients), '-n', str(n), '-P', str(depth), '-r', str(args.keys), '--seed', '76', '--csv'] + list(case.wire())
                                wait_start = time.monotonic()
                                while time.monotonic() < next_launch:
                                    time.sleep(max(0, min(.5, next_launch - time.monotonic())))
                                pacing = time.monotonic() - wait_start
                                next_launch = time.monotonic() + args.clients / args.connections_per_second
                                before = call_count(c, case.argv[0]); start_cpu = cpu(process.pid); start = time.monotonic()
                                run = subprocess.run(argv, capture_output=True, text=True, timeout=180)
                                elapsed = time.monotonic() - start; end_cpu = cpu(process.pid); calls = call_count(c, case.argv[0]) - before
                                stem = '%s-P%d-%s' % (label, depth, case.name)
                                (output / (stem + '.csv')).write_text(run.stdout); (output / (stem + '.stderr')).write_text(run.stderr)
                                assert run.returncode == 0 and 'error' not in (run.stdout + run.stderr).lower(), (argv, run.returncode, run.stderr)
                                parsed = list(csv.DictReader(run.stdout.splitlines())); assert len(parsed) == 1, run.stdout
                                # INFO probes and redis-benchmark startup may themselves issue INFO.
                                extra = 4 if case.argv[0] == 'INFO' else 0
                                assert n <= calls <= math.ceil(n / depth) * depth + extra, (case.name, calls, n, depth)
                                for i in sorted({0, args.keys - 1}): checked(case, c.call(*case.wire(i)), total)
                                rows.append({'round': rep, 'profile': profile, 'scenario': case.name, 'fixture': case.fixture, 'depth': depth, 'requests': n, 'actual_command_calls_including_info_probes': calls, 'cpu_raw': None if start_cpu is None else end_cpu - start_cpu, 'wall_seconds': elapsed, 'pacing_seconds_outside_timing': pacing, 'argv': argv, 'csv': parsed[0]})
                                save_json(output / 'rows.json', rows)
                            print(label, 'P%d' % depth, len(cases), 'scenarios passed', flush=True)
                    final_hll = verify_bank(c, fixtures, args.keys); assert final_hll == hll
                    assert c.call('DBSIZE') == total
                    all_keys = [key(f, i) for f in fixtures for i in range(args.keys)]
                    for start in range(0, len(all_keys), 128): assert c.call('DEL', *all_keys[start:start + 128]) == len(all_keys[start:start + 128])
                    assert c.call('DBSIZE') == 0
                    checks.append({'round': rep, 'profile': profile, 'stage': 'full-bank-and-cleanup', 'keys': total})
                    print(label, 'full model and cleanup passed', flush=True)
                finally: c.close()
            guard(manifest)
    save_json(output / 'checks.json', checks)
    save_json(output / 'summary.json', summarize(rows))
    guard(manifest); (output / 'complete.json').write_text(json.dumps({'complete': True, 'rows': len(rows), 'scenario_count': len(selected), 'command_count': len(metadata['covered_commands']), 'registered_commands': len(registered), 'validate_only': args.validate_only}, indent=2))
    print('complete', len(rows), 'rows;', len(selected), 'scenarios;', len(metadata['covered_commands']), 'commands', flush=True)


if __name__ == '__main__': main()
