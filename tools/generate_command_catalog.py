#!/usr/bin/env python3
"""Capture command metadata facts from an isolated reference Redis server."""
import argparse
import hashlib
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
from integration_test import Client, server

class Simple(bytes):
    pass


class MetadataClient(Client):
    def read(self):
        line = self.stream.readline()
        assert line.endswith(b'\r\n'), line
        kind, data = line[:1], line[1:-2]
        if kind == b'+':
            return Simple(data)
        if kind == b':':
            return int(data)
        if kind == b'$':
            size = int(data)
            if size == -1:
                return None
            value = self.stream.read(size)
            assert self.stream.read(2) == b'\r\n'
            return value
        if kind == b'*':
            size = int(data)
            return None if size == -1 else [self.read() for _ in range(size)]
        raise AssertionError(line)


SUBS = {
    'pubsub': ['help', 'channels', 'numsub', 'numpat',
               'shardchannels', 'shardnumsub'],
    'acl': ['cat', 'deluser', 'dryrun', 'genpass', 'getuser', 'help',
            'list', 'load', 'log', 'save', 'setuser', 'users', 'whoami'],
    'command': ['count', 'info', 'list', 'docs', 'help', 'getkeys',
                'getkeysandflags'],
    'config': ['get', 'set', 'help', 'resetstat'],
    'client': ['id', 'getname', 'setname', 'setinfo', 'kill', 'help', 'info',
               'list', 'unblock'],
    'cluster': ['keyslot', 'slots', 'addslots', 'nodes', 'meet', 'replicate'],
}


def wire(value):
    if value is None:
        return b'*-1\r\n'
    if isinstance(value, Simple):
        return b'+' + value + b'\r\n'
    if isinstance(value, int):
        return b':%d\r\n' % value
    if isinstance(value, str):
        value = value.encode()
    if isinstance(value, bytes):
        return b'$%d\r\n' % len(value) + value + b'\r\n'
    return b'*%d\r\n' % len(value) + b''.join(wire(v) for v in value)


def pairs(value):
    return dict(zip(value[::2], value[1::2]))


def literal(data):
    # Small individual literals preserve the project's 80-column limit.
    out = []
    for start in range(0, len(data), 16):
        part = data[start:start + 16]
        escaped = ''.join('\\x%02x' % byte for byte in part)
        out.append('    "' + escaped + '"')
    return '\n'.join(out)


def generate(binary):
    names = re.findall(r'add\("([A-Z_]+)"',
                       (ROOT / 'action/redis_service.cpp').read_text())
    names = sorted(set(n.lower() for n in names) | {'command'})
    all_names = names + [p + '|' + s for p in SUBS for s in SUBS[p]]
    infos, groups = {}, {}
    with server(binary, redis=True) as port:
        client = MetadataClient(port)
        try:
            for name in all_names:
                info = client.call('COMMAND', 'INFO', name)[0]
                assert info and len(info) == 10 and info[0] == name.encode()
                assert isinstance(info[1], int) and 0 < abs(info[1]) < 65536
                allowed = {name + '|' + sub for sub in SUBS.get(name, [])}
                info[9] = [sub for sub in info[9]
                           if sub[0].decode() in allowed]
                infos[name] = info
                docs = pairs(client.call('COMMAND', 'DOCS', name))
                groups[name] = pairs(docs[name.encode()])[b'group']
        finally:
            client.close()
    for sub in ['syncslots', 'addslave']:
        name = 'cluster|' + sub
        info = [name.encode(), -3, [Simple(b'admin')], 0, 0, 0,
                [b'@admin', b'@slow', b'@dangerous'], [], [], []]
        infos[name] = info
        groups[name] = b'cluster'
        infos['cluster'][9].append(info)
    for info in infos.values():
        info[9].sort(key=lambda child: child[0])
    lines = ['// Generated metadata facts; see '
             'tools/generate_command_catalog.py.',
             '// Reference binary SHA-256:',
             '// ' + hashlib.sha256(Path(binary).read_bytes()).hexdigest(), '']
    entries = []
    for number, name in enumerate(sorted(infos)):
        info = infos[name]
        symbol = 'catalog_' + str(number)
        for suffix, data in [('info', info), ('docs', [
                'summary', 'Redis-compatible ' + name + ' command.',
                'group', groups[name]])]:
            lines += ['static const char ' + symbol + '_' + suffix + '[] =',
                      literal(wire(data)) + ';']
        specs = []
        for spec in info[8]:
            spec = pairs(spec)
            begin = pairs(spec[b'begin_search'])
            find = pairs(spec[b'find_keys'])
            assert begin[b'type'] in (b'index', b'keyword'), name
            search = pairs(begin[b'spec'])
            keyword = 'nullptr'
            if begin[b'type'] == b'keyword':
                token = search[b'keyword'].decode()
                assert re.fullmatch(r'[A-Za-z_]+', token), name
                keyword = '"' + token + '"'
                pos = search[b'startfrom']
            else:
                pos = search[b'index']
            rule = pairs(find[b'spec'])
            counted = find[b'type'] == b'keynum'
            assert counted or find[b'type'] == b'range', name
            a = rule[b'keynumidx'] if counted else rule[b'lastkey']
            b = rule[b'firstkey'] if counted else 0
            step = rule[b'keystep']
            assert not rule.get(b'limit', 0), name
            flags = b' '.join(spec[b'flags']).decode()
            assert re.fullmatch(r'[A-Za-z_ ]*', flags), name
            specs.append('    {"%s", %d, %s, %d, %d, %d, %s}' %
                         (flags, pos, str(counted).lower(), a, b, step,
                          keyword))
        if specs:
            lines += ['static const command_key_rule ' + symbol + '_keys[] = {',
                      ',\n'.join(specs), '};']
        cats = b' '.join(info[6]).decode()
        assert re.fullmatch(r'[@A-Za-z0-9_ -]*', cats), name
        entries.append('    {"%s", %d, "%s",' % (name, info[1], cats))
        entries.append('     %s_info, sizeof(%s_info) - 1,' % (symbol, symbol))
        entries.append('     %s_docs, sizeof(%s_docs) - 1,' % (symbol, symbol))
        entries.append('     %s, %d},' %
                       (symbol + '_keys' if specs else 'nullptr', len(specs)))
        lines.append('')
    lines += ['static const command_catalog_entry catalog[] = {'] + entries
    lines += ['};', '']
    data = '\n'.join(lines)
    # The binary hash comment is deliberately the only longer metadata line.
    assert all(len(line) <= 80 for line in data.splitlines()), [
        line for line in data.splitlines() if len(line) > 80]
    (ROOT / 'common/command_catalog_data.inc').write_text(data)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--redis-server', required=True)
    generate(parser.parse_args().redis_server)


if __name__ == '__main__':
    main()
