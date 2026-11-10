#!/usr/bin/env python3
"""Compare Redis writes on isolated servers and validate the resulting data."""
import argparse
import csv
import ctypes
from dataclasses import dataclass
import platform
from pathlib import Path
import re
import resource
import statistics
import subprocess
import sys
import time

from benchmark_command_matrix import (
    Client, ErrorReply, ROOT, Usage, call_count, cpu, guard,
    input_manifest, running, save_json,
)


DEADLINE = 4102444800123
MARKER = '@key'


@dataclass(frozen=True)
class Case:
    name: str
    command: tuple
    seeds: tuple
    query: tuple
    changed: object
    repeated: bool = False
    shared: tuple = ()
    shared_checks: tuple = ()
    check_value: bytes = None
    read_reply: object = None


def wire(command, key):
    return tuple(key if arg == MARKER else arg for arg in command)


def transition(before, after):
    def count(value):
        assert type(value) is type(before) or type(value) is type(after), value
        assert value == before or value == after, value
        return int(value == after)
    return count


def unordered(before, after):
    count = transition(sorted(before), sorted(after))
    return lambda value: count(sorted(value))


def unchanged(expected, unordered_reply=False):
    def check(value):
        assert type(value) is type(expected), value
        assert (sorted(value) if unordered_reply else value) == expected, value
        return 0
    return check


def hash_deleted(value):
    assert isinstance(value, list) and len(value) in (2, 4), value
    fields = dict(zip(value[::2], value[1::2]))
    assert fields in ({b'a': b'1', b'b': b'2'}, {b'b': b'2'}), fields
    return int(b'a' not in fields)


def integer(value):
    assert isinstance(value, bytes) and int(value) <= 0, value
    return -int(value)


def hash_count(value, step):
    assert isinstance(value, list) and len(value) == 4, value
    fields = dict(zip(value[::2], value[1::2]))
    assert fields[b'keep'] == b'x'
    n = float(fields[b'counter']) / step
    assert n >= 0 and n.is_integer(), value
    return int(n)


def zset_count(value):
    assert isinstance(value, list) and value[:3] == [b'keep', b'-1', b'counter'], value
    assert len(value) == 4 and float(value[3]) >= 0 and float(value[3]).is_integer()
    return int(float(value[3]))


def list_growth(value, left):
    assert isinstance(value, list) and value
    assert (value[-1] if left else value[0]) == b'base'
    assert all(x == b'item' for x in (value[:-1] if left else value[1:]))
    return len(value) - 1


def list_pops(value):
    assert isinstance(value, list) and len(value) <= 32 and value == [b'item'] * len(value)
    return 32 - len(value)


def zset_pops(value):
    assert isinstance(value, list) and len(value) % 2 == 0 and len(value) <= 64
    expected = [v for i in range(len(value) // 2) for v in
                (('member:%02d' % i).encode(), str(i).encode())]
    assert value == expected
    return 32 - len(value) // 2


def geo_added(value):
    assert isinstance(value, list) and len(value) == 1, value
    if value == [None]:
        return 0
    assert len(value[0]) == 2 and all(abs(float(x)) < .00001 for x in value[0]), value
    return 1


def stream_growth(value):
    assert isinstance(value, list) and value[0] == [b'1-0', [b'f', b'base']]
    previous = (1, 0)
    for ident, fields in value[1:]:
        current = tuple(map(int, ident.split(b'-')))
        assert current > previous and fields == [b'f', b'item']
        previous = current
    return len(value) - 1


def cases():
    k = MARKER
    set_value = lambda value: ((('SET', k, value), b'OK'),)
    hset = ((('HSET', k, 'counter', 0, 'keep', 'x'), 2),)
    zset = ((('ZADD', k, -1, 'keep', 0, 'counter'), 2),)
    push = ((('RPUSH', k, 'base'), 1),)
    ttl = ((('PEXPIREAT', k, DEADLINE), 1),)
    pair_zset = ((('ZADD', k, 1, 'a', 2, 'b'), 2),)
    bitmap = b'\x00' * 2048
    zpop = tuple(x for i in range(32) for x in (i, 'member:%02d' % i))
    return [
        Case('decr', ('DECR', k), set_value('0'), ('GET', k), integer, True),
        Case('hincrby', ('HINCRBY', k, 'counter', 1), hset, ('HGETALL', k),
             lambda v: hash_count(v, 1), True),
        Case('hincrbyfloat', ('HINCRBYFLOAT', k, 'counter', '1.25'), hset,
             ('HGETALL', k), lambda v: hash_count(v, 1.25), True),
        Case('zincrby', ('ZINCRBY', k, 1, 'counter'), zset,
             ('ZRANGE', k, 0, -1, 'WITHSCORES'), zset_count, True),
        Case('lpushx', ('LPUSHX', k, 'item'), push, ('LRANGE', k, 0, -1),
             lambda v: list_growth(v, True), True),
        Case('rpushx', ('RPUSHX', k, 'item'), push, ('LRANGE', k, 0, -1),
             lambda v: list_growth(v, False), True),
        Case('lmpop', ('LMPOP', 1, k, 'LEFT'), ((('RPUSH', k, *(['item'] * 32)), 32),),
             ('LRANGE', k, 0, -1), list_pops, True),
        Case('zpopmax', ('ZPOPMAX', k), ((('ZADD', k, *zpop), 32),),
             ('ZRANGE', k, 0, -1, 'WITHSCORES'), zset_pops, True),
        Case('xadd.append', ('XADD', k, '*', 'f', 'item'), ((('XADD', k, '1-0', 'f', 'base'), b'1-0'),),
             ('XRANGE', k, '-', '+'), stream_growth, True),
        Case('setrange.grow', ('SETRANGE', k, 64, 'xyz'), set_value('abc'),
             ('GET', k), transition(b'abc', b'abc' + b'\x00' * 61 + b'xyz')),
        Case('setbit.page', ('SETBIT', k, 8199, 1), set_value(bitmap),
             ('GET', k), transition(bitmap, bitmap[:1024] + b'\x01' + bitmap[1025:])),
        Case('getdel', ('GETDEL', k), set_value('abc'), ('GET', k), transition(b'abc', None)),
        Case('del', ('DEL', k), set_value('abc'), ('GET', k), transition(b'abc', None)),
        Case('hdel', ('HDEL', k, 'a'), ((('HSET', k, 'a', '1', 'b', '2'), 2),),
             ('HGETALL', k), hash_deleted),
        Case('srem', ('SREM', k, 'a'), ((('SADD', k, 'a', 'b'), 2),),
             ('SMEMBERS', k), unordered([b'a', b'b'], [b'b'])),
        Case('zrem', ('ZREM', k, 'a'), pair_zset, ('ZRANGE', k, 0, -1),
             transition([b'a', b'b'], [b'b'])),
        Case('pexpireat', ('PEXPIREAT', k, DEADLINE), set_value('abc'),
             ('PEXPIRETIME', k), transition(-1, DEADLINE), check_value=b'abc'),
        Case('persist', ('PERSIST', k), set_value('abc') + ttl,
             ('PEXPIRETIME', k), transition(DEADLINE, -1), check_value=b'abc'),
        Case('pfadd', ('PFADD', k, 'beta'), ((('PFADD', k, 'alpha'), 1),),
             ('PFCOUNT', k), transition(1, 2)),
        Case('copy.create', ('COPY', 'fixed:a', k), (), ('GET', k), transition(None, b'abc'),
             shared=((('SET', 'fixed:a', 'abc'), b'OK'),),
             shared_checks=((('GET', 'fixed:a'), b'abc'),)),
        Case('bitop.create', ('BITOP', 'AND', k, 'fixed:a', 'fixed:b'), (), ('GET', k),
             transition(None, b'\x0a' * 2048),
             shared=((('SET', 'fixed:a', b'\xaa' * 2048), b'OK'), (('SET', 'fixed:b', b'\x0f' * 2048), b'OK')),
             shared_checks=((('GET', 'fixed:a'), b'\xaa' * 2048), (('GET', 'fixed:b'), b'\x0f' * 2048))),
        Case('sinterstore.create', ('SINTERSTORE', k, 'fixed:a', 'fixed:b'), (), ('SMEMBERS', k),
             unordered([], [b'b']),
             shared=((('SADD', 'fixed:a', 'a', 'b'), 2), (('SADD', 'fixed:b', 'b', 'c'), 2)),
             shared_checks=((('SCARD', 'fixed:a'), 2), (('SCARD', 'fixed:b'), 2),
                            (('SMISMEMBER', 'fixed:a', 'a', 'b'), [1, 1]),
                            (('SMISMEMBER', 'fixed:b', 'b', 'c'), [1, 1]))),
        Case('zunionstore.create', ('ZUNIONSTORE', k, 2, 'fixed:a', 'fixed:b'), (),
             ('ZRANGE', k, 0, -1, 'WITHSCORES'),
             transition([], [b'a', b'1', b'c', b'4', b'b', b'5']),
             shared=((('ZADD', 'fixed:a', 1, 'a', 2, 'b'), 2), (('ZADD', 'fixed:b', 3, 'b', 4, 'c'), 2)),
             shared_checks=((('ZRANGE', 'fixed:a', 0, -1, 'WITHSCORES'), [b'a', b'1', b'b', b'2']),
                            (('ZRANGE', 'fixed:b', 0, -1, 'WITHSCORES'), [b'b', b'3', b'c', b'4']))),
        Case('geoadd.create', ('GEOADD', k, 0, 0, 'here'), (), ('GEOPOS', k, 'here'), geo_added),
    ]


def algebra_cases():
    def member(i):
        return 'member:%04d:value' % i

    def members(values):
        return [value.encode() for value in sorted(values)]

    def scored(values):
        return [part for key in sorted(values, key=lambda key: (values[key], key))
                for part in (key.encode(), str(values[key]).encode())]

    def zadd(key, values):
        arguments = tuple(part for name, score in values.items() for part in (score, name))
        return (('ZADD', key, *arguments), len(values))

    k, a, b = MARKER, 'fixed:a', 'fixed:b'
    result = []
    for size in (32, 192):
        left = {member(i): i for i in range(size)}
        right = {member(i): 2 * i for i in range(size // 2, size + size // 2)}
        difference = left.keys() - right.keys()
        intersection = left.keys() & right.keys()
        union = left.keys() | right.keys()
        limit = 8 if size == 32 else 32
        set_query = ('SMEMBERS', k)
        zset_query = ('ZRANGE', k, 0, -1, 'WITHSCORES')
        set_source = unchanged(members(left), True)
        zset_source = unchanged(scored(left))
        set_seeds = ((('SADD', k, *left), size),)
        zset_seeds = (zadd(k, left),)
        set_right = ((('SADD', b, *right), size),)
        zset_right = (zadd(b, right),)
        set_checks = lambda key, values: ((('SCARD', key), len(values)),
            (('SMISMEMBER', key, *values), [1] * len(values)))
        zset_checks = lambda key, values: ((('ZRANGE', key, 0, -1, 'WITHSCORES'), scored(values)),)
        for command, output in (('SDIFF', difference), ('SINTER', intersection), ('SUNION', union)):
            name = command.lower()
            result.append(Case(f'{name}.{size}', (command, k, b), set_seeds, set_query,
                set_source, shared=set_right, shared_checks=set_checks(b, right),
                read_reply=unchanged(members(output), True)))
            result.append(Case(f'{name}store.{size}', (command + 'STORE', k, a, b), (),
                set_query, unordered([], members(output)),
                shared=((('SADD', a, *left), size),) + set_right,
                shared_checks=set_checks(a, left) + set_checks(b, right)))
        result.append(Case(f'sintercard.{size}', ('SINTERCARD', 2, k, b, 'LIMIT', limit),
            set_seeds, set_query, set_source, shared=set_right,
            shared_checks=set_checks(b, right), read_reply=unchanged(limit)))
        for command, names in (('ZDIFF', difference), ('ZINTER', intersection), ('ZUNION', union)):
            name = command.lower()
            weights = () if command == 'ZDIFF' else ('WEIGHTS', 2, 3)
            scores = ({key: left[key] for key in names} if not weights else
                      {key: 2 * left.get(key, 0) + 3 * right.get(key, 0) for key in names})
            output = scored(scores)
            result.append(Case(f'{name}.{size}', (command, 2, k, b, *weights, 'WITHSCORES'),
                zset_seeds, zset_query, zset_source, shared=zset_right,
                shared_checks=zset_checks(b, right), read_reply=unchanged(output)))
            result.append(Case(f'{name}store.{size}', (command + 'STORE', k, 2, a, b, *weights),
                (), zset_query, transition([], output), shared=(zadd(a, left),) + zset_right,
                shared_checks=zset_checks(a, left) + zset_checks(b, right)))
        result.append(Case(f'zintercard.{size}', ('ZINTERCARD', 2, k, b, 'LIMIT', limit),
            zset_seeds, zset_query, zset_source, shared=zset_right,
            shared_checks=zset_checks(b, right), read_reply=unchanged(limit)))
        start, end = size // 4, size // 2 - 1
        selected = {key: score for key, score in left.items() if start <= score <= end}
        result.append(Case(f'zrangestore.{size}', ('ZRANGESTORE', k, a, start, end),
            (), zset_query, transition([], scored(selected)), shared=(zadd(a, left),),
            shared_checks=zset_checks(a, left)))
        equal = dict.fromkeys(left, 0)
        for command, arguments, values, removed in (
                ('ZREMRANGEBYLEX', ('[' + member(start), '[' + member(end)), equal, selected),
                ('ZREMRANGEBYRANK', (size - limit, -1), left,
                    {key for key, score in left.items() if score >= size - limit}),
                ('ZREMRANGEBYSCORE', (start, end), left, selected)):
            kept = {key: score for key, score in values.items() if key not in removed}
            result.append(Case(f'{command.lower()}.{size}', (command, k, *arguments),
                (zadd(k, values),), zset_query, transition(scored(values), scored(kept))))
    return result


def pipelined(client, commands):
    for start in range(0, len(commands), 64):
        group = commands[start:start + 64]
        client.socket.sendall(b''.join(Client.encode(args) for args in group))
        for args in group:
            reply = client.read()
            assert not isinstance(reply, ErrorReply), (args, reply)
            yield reply


def checked(client, exchanges):
    for actual, (_, expected) in zip(pipelined(client, [args for args, _ in exchanges]), exchanges):
        assert type(actual) is type(expected) and actual == expected, (actual, expected)


def snapshot(client, pid):
    info = dict(line.split(b':', 1) for line in client.call('INFO', 'memory').splitlines() if b':' in line)
    samples = []
    if sys.platform == 'darwin':
        rusage = ctypes.CDLL('/usr/lib/libproc.dylib', use_errno=True).proc_pid_rusage
        rusage.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_void_p]
        rusage.restype = ctypes.c_int
        for _ in range(5):
            result = Usage()
            assert rusage(pid, 2, ctypes.byref(result)) == 0
            samples.append((result.rss, result.footprint))
            time.sleep(.005)
    elif sys.platform.startswith('linux'):
        for _ in range(5):
            status = Path('/proc/%d/status' % pid).read_text().splitlines()
            rss = int(next(line for line in status if line.startswith('VmRSS:')).split()[1]) * 1024
            samples.append((rss, None))
            time.sleep(.005)
    return dict(heap=int(info[b'used_memory']), allocator=info[b'mem_allocator'].decode(),
                rss=statistics.median(x[0] for x in samples) if samples else None,
                footprint=statistics.median(x[1] for x in samples) if samples and samples[0][1] is not None else None,
                samples=samples)


def verify(client, case, keys):
    mutations = [case.changed(value) for value in pipelined(client, [wire(case.query, k) for k in keys])]
    if case.read_reply is not None:
        for value in pipelined(client, [wire(case.command, k) for k in keys]):
            case.read_reply(value)
    checked(client, case.shared_checks)
    if case.check_value is not None:
        checked(client, [(('GET', k), case.check_value) for k in keys])
    return sum(mutations), sum(value != 0 for value in mutations)


def benchmark(args, output, label, profile, binary, backend, case, depth, next_launch):
    keys = ['write:%012d' % i for i in range(args.keys)]
    n = args.requests if case.repeated or case.read_reply is not None else args.keys
    with running(binary, backend, output, label) as (port, process):
        client = Client(port)
        client.socket.settimeout(60)
        try:
            checked(client, [(('PING',), b'PONG')] * 64)
            phases = {'idle': snapshot(client, process.pid)}
            checked(client, case.shared)
            checked(client, [(wire(command, k), expected) for k in keys for command, expected in case.seeds])
            assert verify(client, case, keys) == (0, 0)
            phases['loaded'] = snapshot(client, process.pid)
            argv = [str(args.redis_benchmark), '-h', '127.0.0.1', '-p', str(port),
                    '-c', str(args.clients), '-n', str(n), '-P', str(depth), '-r',
                    str(args.keys), '--seed', '76', '--csv', *map(str, wire(case.command, 'write:__rand_int__'))]
            time.sleep(max(0, next_launch - time.monotonic()))
            next_launch = time.monotonic() + args.clients / args.connections_per_second
            command = case.command[0]
            before = call_count(client, command)
            start_cpu = cpu(process.pid)
            start = time.monotonic()
            result = subprocess.run(argv, capture_output=True, text=True, timeout=180)
            elapsed = time.monotonic() - start
            end_cpu = cpu(process.pid)
            calls = call_count(client, command) - before
            (output / (label + '.csv')).write_text(result.stdout)
            (output / (label + '.stderr')).write_text(result.stderr)
            assert result.returncode == 0 and 'error' not in (result.stdout + result.stderr).lower(), (argv, result)
            records = list(csv.DictReader(result.stdout.splitlines()))
            assert len(records) == 1 and calls == n, (label, calls, n)
            phases['written'] = snapshot(client, process.pid)
            changed, touched = verify(client, case, keys)
            if case.read_reply is not None:
                assert changed == touched == 0, label
            elif case.repeated:
                assert changed == n, (label, changed, n)
            else:
                assert changed == touched and .5 * n <= changed <= n, (label, changed, n)
            all_keys = keys + [command[1] for command, _ in case.shared]
            present = client.call('DBSIZE')
            deleted = 0
            for start in range(0, len(all_keys), 128):
                deleted += client.call('DEL', *all_keys[start:start + 128])
            assert deleted == present and client.call('DBSIZE') == 0
            phases['empty'] = snapshot(client, process.pid)
        finally:
            client.close()
    return dict(profile=profile, scenario=case.name, command=case.command[0], depth=depth,
                calls=calls, changed=changed, touched=touched, keys=len(keys), phases=phases,
                cpu_raw=None if start_cpu is None else end_cpu - start_cpu,
                wall_seconds=elapsed, argv=argv, csv=records[0]), next_launch


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pkv', type=Path, required=True)
    parser.add_argument('--tbb-pkv', type=Path)
    parser.add_argument('--redis-server', type=Path, required=True)
    parser.add_argument('--redis-benchmark', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--keys', type=int, default=4096)
    parser.add_argument('--requests', type=int, default=32768)
    parser.add_argument('--rounds', type=int, default=3)
    parser.add_argument('--clients', type=int, default=50)
    parser.add_argument('--connections-per-second', type=float, default=100)
    parser.add_argument('--only', nargs='+')
    parser.add_argument('--suite', choices=('mutations', 'algebra'), default='mutations')
    parser.add_argument('--extra-input', type=Path, action='append', default=[])
    args = parser.parse_args()
    if (args.keys < 64 or args.keys % 64 or not 64 <= args.requests <= 8 * args.keys or
            args.requests % 64 or args.rounds < 1 or args.clients < 1 or args.connections_per_second <= 0):
        parser.error('keys and requests must be positive multiples of 64, and requests at most eight per key')
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    profiles = [('mdb', args.pkv.resolve(), 'mdb')]
    if args.tbb_pkv:
        profiles.append(('tbb', args.tbb_pkv.resolve(), 'mdb_tbb'))
    profiles.append(('redis', args.redis_server.resolve(), 'redis'))
    args.redis_benchmark = args.redis_benchmark.resolve()
    selected = algebra_cases() if args.suite == 'algebra' else cases()
    if args.only:
        if set(args.only) - {case.name for case in selected}:
            parser.error('unknown scenario')
        selected = [case for case in selected if case.name in args.only]
    registered = set(re.findall(r'\badd\("([A-Z][A-Z0-9_]*)"', (ROOT / 'action/redis_service.cpp').read_text()))
    commands = sorted({case.command[0] for case in selected})
    assert set(commands) <= registered
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    proof = input_manifest([p[1] for p in profiles] + [args.redis_benchmark, __file__] + args.extra_input)
    save_json(output / 'inputs.json', proof)
    save_json(output / 'manifest.json', dict(argv=sys.argv, platform=platform.platform(),
              profiles=[dict(name=p, binary=str(b), backend=k, workers=1) for p, b, k in profiles],
              scenarios=[c.name for c in selected], commands=commands, registered=len(registered)))
    rows, next_launch = [], 0.0
    for rep in range(args.rounds):
        order = profiles[rep % len(profiles):] + profiles[:rep % len(profiles)]
        for profile, binary, backend in order:
            guard(proof)
            for depth in ((1, 64) if rep % 2 == 0 else (64, 1)):
                ordered = selected if rep % 2 == 0 else list(reversed(selected))
                for case in ordered:
                    label = f'r{rep}-{profile}-P{depth}-{case.name}'
                    row, next_launch = benchmark(args, output, label, profile, binary,
                                                 backend, case, depth, next_launch)
                    rows.append(dict(row, round=rep))
                    save_json(output / 'rows.json', rows)
                print(f'r{rep}-{profile} P{depth} {len(ordered)} scenarios validated', flush=True)
            guard(proof)
    save_json(output / 'complete.json', dict(rows=len(rows), profiles=len(profiles),
              rounds=args.rounds, scenarios=len(selected)))


if __name__ == '__main__':
    main()
