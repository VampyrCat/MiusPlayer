"""Compares unit positions of two running game instances: live_diff.py <pid A> <pid B> [samples]"""
import math, struct, sys, time
import objs, peek

def units(h):
    out = {}
    for p, name, _ in objs.objects(h):
        if name.startswith(('i_techn', 'i_human')):
            d = peek.read(h, p + 0x1b0, 12)
            if d: out[name] = struct.unpack('<3f', d)
    return out

ha, hb = peek.open_proc(int(sys.argv[1])), peek.open_proc(int(sys.argv[2]))
for sample in range(int(sys.argv[3]) if len(sys.argv) > 3 else 3):
    a, b = units(ha), units(hb)
    for kind in ('i_techn', 'i_human'):
        d = sorted(math.dist(a[n], b[n]) for n in a if n in b and n.startswith(kind))
        if d:
            print('%s: %3d units, mean difference %.2f m, median %.2f m, largest %.2f m, within 1 m: %d' % (
                kind, len(d), sum(d) / len(d), d[len(d) // 2], d[-1], sum(x < 1 for x in d)))
    time.sleep(4)
