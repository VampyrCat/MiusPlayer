"""Turns an operation save round: mirror_opsave.py <in.opsave> <out.opsave>

Every formation ("pool") and battle group in an operation save carries an `ai` tag: 'USER' for the ones the
player commands, a computer group code ('EN1', 'U286', ...) for the others. This exchanges 'USER' with the
(single) computer code of the opposing side, in every cell of the file, and prints what it did.
"""
import struct
import sys
import collections

import azcf


def main():
    a = azcf.Azcf(open(sys.argv[1], 'rb').read())
    user = struct.unpack('<I', b'USER')[0]
    sides = {}          # side code -> Counter of ai tags, from the pools
    for i in range(a.count):
        n = a.node(i)
        if n['type'] != 1 or not n['name'].startswith('p_'):
            continue
        f = {}
        for r in range(n['rows']):
            o = n['data'] + r * 0x40
            f[a.b[o + 0x20:o + 0x40].split(b'\0')[0].decode()] = struct.unpack_from('<I', a.b, o)[0]
        if f.get('ai'):
            sides.setdefault(f.get('side'), collections.Counter())[f['ai']] += 1
    enemy_codes = sorted({c for s in sides.values() for c in s if c != user})
    print('ai tags by side:', {struct.pack('<I', s).rstrip(b'\0').decode(): {struct.pack('<I', c).rstrip(b'\0').decode(): k for c, k in v.items()} for s, v in sides.items()})
    if len(enemy_codes) != 1:
        print('this save has %d computer codes; only one is handled' % len(enemy_codes))
        return 1
    enemy = enemy_codes[0]
    swapped = 0
    for i in range(a.count):
        n = a.node(i)
        cells = []
        if n['type'] == 1:
            cells = [n['data'] + r * 0x40 for r in range(n['rows'])]
        elif n['type'] == 2:
            cells = [n['data'] + k * 0x20 for k in range(n['rows'] * n['cols'])]
        for o in cells:
            v, kind = struct.unpack_from('<I', a.b, o)[0], struct.unpack_from('<I', a.b, o + 16)[0]
            if kind != 4:
                continue
            if v == user:
                struct.pack_into('<I', a.b, o, enemy); swapped += 1
            elif v == enemy:
                struct.pack_into('<I', a.b, o, user); swapped += 1
    open(sys.argv[2], 'wb').write(a.b)
    print('%d tags exchanged' % swapped)
    return 0


if __name__ == '__main__':
    sys.exit(main())
