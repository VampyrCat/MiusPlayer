"""Do units of the other player flicker in and out of view?  flicker_probe.py <pid> <own unit prefix> [samples] [ms between]

Samples the visibility values of every unit that is not this game's own, many times a second, and counts
how often each value changes. A unit nobody has spotted must stay hidden: its "seen" countdown (+0x72c)
below zero and its drawn flag off (soldier +0x228c, vehicle +0x1e66) the whole time.
"""
import collections
import struct
import sys
import time

import objs
import peek

HUMAN = [('timer>0', 0x72c, 'f'), ('drawn +0x228c', 0x228c, 'B'), ('+0x228d', 0x228d, 'B'), ('+0x228e', 0x228e, 'B'), ('+0x745', 0x745, 'B'), ('+0x749', 0x749, 'B')]
TECHN = [('timer>0', 0x72c, 'f'), ('drawn +0x1e66', 0x1e66, 'B'), ('+0x1dd0', 0x1dd0, 'i'), ('+0x1e5f', 0x1e5f, 'B'), ('+0x745', 0x745, 'B'), ('+0x749', 0x749, 'B')]


def read(h, p, fields):
    out = []
    for _, off, fmt in fields:
        d = peek.read(h, p + off, struct.calcsize(fmt))
        v = struct.unpack('<' + fmt, d)[0] if d else None
        out.append(v > 0 if fmt == 'f' and v is not None else v)
    return tuple(out)


def main():
    h = peek.open_proc(int(sys.argv[1]))
    own = sys.argv[2]
    samples = int(sys.argv[3]) if len(sys.argv) > 3 else 300
    pause = (float(sys.argv[4]) if len(sys.argv) > 4 else 20) / 1000
    units = [(p, name) for p, name, _ in objs.objects(h)
             if (name.startswith('i_human') or name.startswith('i_techn')) and not name.rsplit('_', 1)[1].startswith(own)]
    last = {}
    changes = collections.Counter()
    seen_drawn = set()
    for s in range(samples):
        for p, name in units:
            fields = HUMAN if name.startswith('i_human') else TECHN
            v = read(h, p, fields)
            if v[1]:
                seen_drawn.add(name)
            if name in last:
                for (label, _, _), a, b in zip(fields, last[name], v):
                    if a != b:
                        changes[(name[:7], label)] += 1
                        changes[('unit', name)] += 1
            last[name] = v
        time.sleep(pause)
    print('%d units of the other player watched for %.1f s; %d of them were drawn at some moment' % (len(units), samples * pause, len(seen_drawn)))
    for (kind, label), n in sorted(changes.items()):
        if kind != 'unit':
            print('   %s %-16s changed %d times' % (kind, label, n))
    worst = sorted(((n, name) for (kind, name), n in changes.items() if kind == 'unit'), reverse=True)[:5]
    for n, name in worst:
        print('   most restless: %s, %d changes; now %s' % (name, n, last[name]))
    flips = sum(n for (kind, label), n in changes.items() if kind != 'unit' and label.startswith('drawn'))
    print('drawn flag flips: %d' % flips)


if __name__ == '__main__':
    main()
