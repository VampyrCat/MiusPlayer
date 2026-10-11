"""Fog of war in two running games: fog_probe.py <pid A> <pid B> [samples] [seconds between] [fields]

A unit is drawn while its "seen" countdown (object +0x72c, seconds) is above zero. The game's visibility
manager sets it to 10 for the player's own units every frame and for an opponent's unit whenever one of the
player's units has spotted it; the unit's own update counts it down.

For each game this prints how many of the other player's units are currently shown. With "fields" it also
lists, for every saved field of the base unit (object +0x680..+0x752), in how many units the two games
hold different values - split by whose units they are - to tell per-player fields from battle state.
"""
import struct
import sys
import time

import objs
import peek

FIELDS = [(0x680, 16), (0x690, 16), (0x6a0, 16), (0x6b0, 4), (0x6b4, 4), (0x6e8, 4), (0x6ec, 4), (0x6f0, 4), (0x6f4, 4), (0x6f8, 4),
          (0x6fc, 4), (0x700, 4), (0x704, 4), (0x710, 4), (0x714, 4), (0x71c, 4), (0x724, 4), (0x728, 4), (0x72c, 4), (0x730, 4),
          (0x734, 4), (0x738, 4), (0x73c, 4), (0x740, 4)] + [(o, 1) for o in range(0x744, 0x753)]


def units(h):
    out = {}
    for p, name, _ in objs.objects(h):
        if name.startswith('i_human') or name.startswith('i_techn'):
            d = peek.read(h, p + 0x680, 0xd8)
            if d:
                out[name] = d
    return out


def f32(d, off):
    return struct.unpack_from('<f', d, off - 0x680)[0]


def own(d):
    return d[0x6f0 - 0x680] != 0


def main():
    ha, hb = peek.open_proc(int(sys.argv[1])), peek.open_proc(int(sys.argv[2]))
    samples = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    pause = float(sys.argv[4]) if len(sys.argv) > 4 else 10
    fields = len(sys.argv) > 5
    worst = 0
    for s in range(samples):
        a, b = units(ha), units(hb)
        line = []
        for label, mine in (('A', a), ('B', b)):
            theirs = [n for n in mine if not own(mine[n])]
            shown = [n for n in theirs if f32(mine[n], 0x72c) > 0]
            worst = max(worst, len(shown))
            line.append('%s: %d own, %d of the other player, %d of those shown' % (label, len(mine) - len(theirs), len(theirs), len(shown)))
        print('sample %d   %s' % (s + 1, '   '.join(line)))
        if fields:
            common = sorted(set(a) & set(b))
            host = [n for n in common if own(a[n])]
            join = [n for n in common if own(b[n])]
            print('   field      differs in host-owned %d   joiner-owned %d    example (A / B)' % (len(host), len(join)))
            for off, size in FIELDS:
                lo = off - 0x680
                dh = [n for n in host if a[n][lo:lo + size] != b[n][lo:lo + size]]
                dj = [n for n in join if a[n][lo:lo + size] != b[n][lo:lo + size]]
                if not dh and not dj:
                    continue
                n = (dh + dj)[0]
                va, vb = a[n][lo:lo + size], b[n][lo:lo + size]
                if size == 4:
                    ex = '%08x (%.3g) / %08x (%.3g)' % (struct.unpack('<I', va)[0], struct.unpack('<f', va)[0],
                                                       struct.unpack('<I', vb)[0], struct.unpack('<f', vb)[0])
                else:
                    ex = '%s / %s' % (va[:8].hex(), vb[:8].hex())
                print('   +0x%03x/%-2d  %4d  %4d   %s' % (off, size, len(dh), len(dj), ex))
        if s + 1 < samples:
            time.sleep(pause)
    print('most units of the other player shown at once: %d' % worst)


if __name__ == '__main__':
    main()
