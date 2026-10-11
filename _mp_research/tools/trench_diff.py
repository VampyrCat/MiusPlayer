"""Do two running games have the same trenches?   trench_diff.py <pid host> <pid joiner>

The trench manager (i_trencher) keeps its trenches by map cell: 0x900 cells (48 x 48) of 0x40 bytes from
object +0x2f0, each with a list of 0x70-byte records (pointer at cell +0x20, count at cell +0x2c); a record
starts with the trench's position (x, height, z) and its following floats describe how it lies. The script lists how many trenches each game has and how
many of them the other game has in the same place lying the same way.
"""
import struct
import sys

import objs
import peek


def trenches(pid):
    h = peek.open_proc(pid)
    tr = [p for p, n, _ in objs.objects(h) if n.startswith('i_trencher')]
    if not tr:
        return None
    out = []
    cells = peek.read(h, tr[0] + 0x2f0, 0x900 * 0x40)
    if not cells:
        return None
    for k in range(0x900):
        ptr, _, count, cap = struct.unpack_from('<QIII', cells, k * 0x40 + 0x20)
        if not ptr or count == 0 or count > 4096 or count > cap:
            continue
        data = peek.read(h, ptr, count * 0x70)
        if not data:
            continue
        for i in range(count):
            f = struct.unpack_from('<12f', data, i * 0x70)
            if all(abs(x) < 1e7 for x in f[:3]) and (abs(f[0]) > 0.01 or abs(f[2]) > 0.01):
                out.append(f)
    return out


def key(f, digits):
    return tuple(round(x, digits) for x in f[:3])


def main():
    a, b = trenches(int(sys.argv[1])), trenches(int(sys.argv[2]))
    if a is None or b is None:
        print('no trench manager found')
        return
    print('host game: %d trenches, joiner game: %d trenches' % (len(a), len(b)))
    for digits, label in ((1, 'within 10 cm'), (0, 'within 1 m')):
        ka, kb = {}, {}
        for f in a:
            ka.setdefault(key(f, digits), []).append(f)
        for f in b:
            kb.setdefault(key(f, digits), []).append(f)
        same_place = sum(min(len(ka[k]), len(kb[k])) for k in ka if k in kb)
        same_lie = 0
        for k in ka:
            if k in kb:
                for fa, fb in zip(ka[k], kb[k]):
                    if all(abs(x - y) < 0.02 for x, y in zip(fa[4:12], fb[4:12])):
                        same_lie += 1
        print('   in the same place %s: %d (of those lying the same way: %d); only in the host game %d, only in the joiner game %d' % (
            label, same_place, same_lie, len(a) - same_place, len(b) - same_place))
    there = {key(g, 0) for g in b}
    only = [f for f in a if key(f, 0) not in there][:4]
    for f in only:
        print('      host only: %.1f %.1f %.1f' % f[:3])
    ok = len(a) == len(b) and len(a) > 0 and all(key(f, 0) in there for f in a)
    print('trenches: %s' % ('the same in both games' if ok else 'none to compare' if not a and not b else 'DIFFERENT'))


if __name__ == '__main__':
    main()
