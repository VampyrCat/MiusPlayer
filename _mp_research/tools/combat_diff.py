"""Compares combat state between two running games: combat_diff.py <pid A> <pid B> [samples] [seconds between]

For every unit (vehicle or soldier) it reads, from both games, whether the object still exists, its
status bytes and its position. A unit "disagrees" when it exists in only one game or its status differs.
Soldier status bytes are the object fields at +0x228d and +0x228e (the class's own status getters,
virtual slots 6 and 60); vehicle status is the saved-state byte block at +0xd07..+0xd10.
"""
import math
import struct
import sys
import time

import objs
import peek


def units(h):
    out = {}
    for p, name, _ in objs.objects(h):
        if name.startswith('i_human'):
            d = peek.read(h, p + 0x2289, 0x20)
            pos = peek.read(h, p + 0x1b0, 12)
            if d and pos:
                out[name] = (d[4:6], struct.unpack('<3f', pos))
        elif name.startswith('i_techn'):
            d = peek.read(h, p + 0xd07, 10)
            pos = peek.read(h, p + 0x1b0, 12)
            if d and pos:
                # +0xd0d is left out: it differs between two games from the first frame on, even for parked
                # vehicles (it follows the local view, not the battle)
                out[name] = (d[:6] + d[7:], struct.unpack('<3f', pos))
    return out


def main():
    ha, hb = peek.open_proc(int(sys.argv[1])), peek.open_proc(int(sys.argv[2]))
    samples = int(sys.argv[3]) if len(sys.argv) > 3 else 6
    pause = float(sys.argv[4]) if len(sys.argv) > 4 else 20
    worst = 0
    for s in range(samples):
        a, b = units(ha), units(hb)
        only_a = sorted(set(a) - set(b))
        only_b = sorted(set(b) - set(a))
        common = sorted(set(a) & set(b))
        status = [n for n in common if a[n][0] != b[n][0]]
        far = [n for n in common if math.dist(a[n][1], b[n][1]) > 5]
        soldiers_a = sum(n.startswith('i_human') for n in a)
        soldiers_b = sum(n.startswith('i_human') for n in b)
        flagged_a = sum(1 for n in a if n.startswith('i_human') and a[n][0] != bytes(2))
        flagged_b = sum(1 for n in b if n.startswith('i_human') and b[n][0] != bytes(2))
        disagree = len(only_a) + len(only_b) + len(status)
        worst = max(worst, disagree)
        print('sample %d: A %d units (%d soldiers, %d with status set), B %d units (%d soldiers, %d with status set)' % (
            s + 1, len(a), soldiers_a, flagged_a, len(b), soldiers_b, flagged_b))
        print('   disagreeing units: %d  (only in A %d, only in B %d, status differs %d);  more than 5 m apart: %d' % (
            disagree, len(only_a), len(only_b), len(status), len(far)))
        for n in (only_a + only_b + status)[:6]:
            print('      %-26s A %s  B %s' % (n, a[n][0].hex() if n in a else 'gone', b[n][0].hex() if n in b else 'gone'))
        for n in sorted(far, key=lambda n: -math.dist(a[n][1], b[n][1]))[:8]:
            print('      far %-22s %6.1f m   A (%.0f %.0f %.0f)  B (%.0f %.0f %.0f)' % ((n, math.dist(a[n][1], b[n][1])) + a[n][1] + b[n][1]))
        if s + 1 < samples:
            time.sleep(pause)
    print('worst sample: %d disagreeing units' % worst)


if __name__ == '__main__':
    main()
