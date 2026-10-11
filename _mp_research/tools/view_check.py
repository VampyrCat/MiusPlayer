"""Whose units does each game think are its own player's?  view_check.py <pid A> <pid B>

Every unit object carries a flag at +0x6f0: 1 when this game's player commands it. In a versus game the
host's player commands the 0x08... units (first force) and the joining player the 0x10... units, so no
unit may be flagged in both games, and none of the other player's units may be flagged at all (a unit
flagged as "ours" is, among other things, always visible).
"""
import sys

import objs
import peek


def flagged(h):
    out = {}
    for p, name, _ in objs.objects(h):
        if name.startswith('i_human') or name.startswith('i_techn'):
            d = peek.read(h, p + 0x6f0, 1)
            if d:
                out[name] = d[0]
    return out


def side(name):
    return 'first force' if name.rsplit('_', 1)[1].startswith('08') else 'second force'


def main():
    a, b = flagged(peek.open_proc(int(sys.argv[1]))), flagged(peek.open_proc(int(sys.argv[2])))
    for label, units in (('A', a), ('B', b)):
        counts = {}
        for n, f in units.items():
            key = side(n)
            counts.setdefault(key, [0, 0])[1 if f else 0] += 1
        print('%s: ' % label + ', '.join('%s %d own / %d not' % (k, v[1], v[0]) for k, v in sorted(counts.items())))
    both = [n for n in a if a[n] and b.get(n)]
    wrong_a = [n for n in a if a[n] and side(n) == 'second force']
    wrong_b = [n for n in b if b[n] and side(n) == 'first force']
    print('units both games count as their own: %d; other player\'s units counted as own: A %d, B %d' % (
        len(both), len(wrong_a), len(wrong_b)))
    print('view: %s' % ('ok' if not both and not wrong_a and not wrong_b else 'WRONG'))


if __name__ == '__main__':
    main()
