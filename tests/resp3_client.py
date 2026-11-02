"""Typed RESP2/RESP3 test reader. Commands retain Redis bulk-array framing."""
from collections import namedtuple

from integration_test import Client

Reply = namedtuple('Reply', 'kind value')


class RespClient(Client):
    def read(self):
        line = self.stream.readline()
        assert line.endswith(b'\r\n'), line
        kind, value = line[:1], line[1:-2]
        if kind in (b'+', b'-'):
            return Reply(kind, value)
        if kind == b':':
            return Reply(kind, int(value))
        if kind == b',':
            return Reply(kind, float(value))
        if kind == b'_':
            assert value == b''
            return Reply(kind, None)
        if kind in (b'$', b'='):
            length = int(value)
            if length < 0:
                assert kind == b'$' and length == -1
                return Reply(kind, None)
            data = self.stream.read(length)
            assert len(data) == length and self.stream.read(2) == b'\r\n'
            return Reply(kind, data)
        if kind in (b'*', b'%', b'~', b'>'):
            length = int(value)
            if length < 0:
                assert kind == b'*' and length == -1
                return Reply(kind, None)
            return Reply(kind, [self.read() for _ in
                               range(length * (2 if kind == b'%' else 1))])
        raise AssertionError(('unexpected reply type', line))


def canonical(reply):
    kind, value = reply
    if kind == b'%':
        items = [(canonical(k), canonical(v)) for k, v in
                 zip(value[::2], value[1::2]) if k.value != b'notes']
        return kind, tuple(sorted(items, key=repr))
    if kind in (b'~', b'*', b'>') and value is not None:
        items = [canonical(v) for v in value]
        return kind, tuple(sorted(items, key=repr) if kind == b'~' else items)
    return kind, value


def mapping(reply):
    assert reply.kind in (b'%', b'*') and len(reply.value) % 2 == 0
    return {k.value: v for k, v in zip(reply.value[::2], reply.value[1::2])}
