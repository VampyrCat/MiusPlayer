"""List live engine objects of a running game: objs.py <pid> [iface filter]"""
import peek, struct, sys, collections
OBJMAN = 0x7FDEF0

def cstr(h, a, n=64):
    d = peek.read(h, a, n)
    if not d: return ''
    return d.split(b'\0')[0].decode('latin1')

def objects(h):
    d = peek.read(h, OBJMAN + 0xf0, 16)
    arr, _, cnt = struct.unpack('<QII', d)
    ptrs = struct.unpack('<%dQ' % cnt, peek.read(h, arr, cnt * 8))
    out = []
    for p in ptrs:
        hd = peek.read(h, p, 0xf0)
        out.append((p, hd[8:72].split(b'\0')[0].decode('latin1'), struct.unpack_from('<Q', hd, 0)[0]))
    return out

if __name__ == '__main__':
    pid = int(sys.argv[1]); flt = sys.argv[2] if len(sys.argv) > 2 else ''
    h = peek.open_proc(pid); mods = peek.modules(h)
    objs = objects(h)
    c = collections.Counter(n.split(':')[0] for _, n, _ in objs)
    print(len(objs), dict(c) if not flt else '')
    for p, n, vt in objs:
        if flt and flt in n: print('%x %-34s %s' % (p, n, peek.where(mods, vt)))
