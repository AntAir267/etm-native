#!/usr/bin/env python3
"""A text table of the game (Text\\TX<language><name>.bin) as lines: id, then the text. UTF-16: dword 0,
count, then count offsets from +8 (DESIGN.md decision 13). TX<language>: e English, b Brazilian
Portuguese, f, g, i, s, j, k, m, x.

    txdump.py TXeIMT.bin [more.bin ...]
"""
import os, struct, sys

GAME = os.path.expanduser('~/.local/share/etm-native/game/Text/')


def load(path):
    if not os.path.isabs(path):
        path = GAME + path
    d = open(path, 'rb').read()
    n = struct.unpack_from('<I', d, 4)[0]
    out = []
    for o in struct.unpack_from('<%dI' % n, d, 8):
        o += 8
        e = o
        while e + 1 < len(d) and (d[e] or d[e + 1]):
            e += 2
        out.append(d[o:e].decode('utf-16-le', 'replace'))
    return out


if __name__ == '__main__':
    for f in sys.argv[1:]:
        for i, s in enumerate(load(f)):
            print(i, repr(s))
