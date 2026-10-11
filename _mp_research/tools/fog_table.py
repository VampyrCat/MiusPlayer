"""Fog of war in two running games: fog_table.py <pid host> <pid joiner> [host unit prefix]

A unit is drawn while its "seen" countdown (object +0x72c) is above zero. For each game this counts, per
force, how many units are shown and how many hidden. A game always shows its own player's units; of the
other player's it may only show the ones its own units have spotted.
"""
import collections
import struct
import sys

import objs
import peek


def table(pid, host_prefix):
    h = peek.open_proc(pid)
    c = collections.Counter()
    for p, name, _ in objs.objects(h):
        if name.startswith('i_human') or name.startswith('i_techn'):
            d = peek.read(h, p + 0x72c, 4)
            if not d:
                continue
            force = 'host' if name.rsplit('_', 1)[1].startswith(host_prefix) else 'joiner'
            c[(force, struct.unpack('<f', d)[0] > 0)] += 1
    return c


def main():
    prefix = sys.argv[3] if len(sys.argv) > 3 else '08'
    for label, pid in (('host game  ', int(sys.argv[1])), ('joiner game', int(sys.argv[2]))):
        c = table(pid, prefix)
        print('%s: host\'s units %d shown / %d hidden, joiner\'s units %d shown / %d hidden' % (
            label, c[('host', True)], c[('host', False)], c[('joiner', True)], c[('joiner', False)]))


if __name__ == '__main__':
    main()
