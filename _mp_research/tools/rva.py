# Bytes of a game module at given RVAs, following a leading jmp (handler stubs are often a bare `jmp rel32`).
#   python rva.py <module name, e.g. adv_game> <rva hex>...
import struct, sys

def load(name):
    path = r"G:/GRAVITEAMMODDING/Graviteam Tactics Mius-Front/bin/rel/x64/%s.codelib" % name
    d = open(path, 'rb').read()
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    nsec = struct.unpack_from('<H', d, pe + 6)[0]
    opt = struct.unpack_from('<H', d, pe + 20)[0]
    secs = []
    for i in range(nsec):
        o = pe + 24 + opt + i * 40
        vs, va, rs, ro = struct.unpack_from('<IIII', d, o + 8)
        secs.append((va, vs, ro, rs))
    return d, secs

def off(secs, rva):
    for va, vs, ro, rs in secs:
        if va <= rva < va + max(vs, rs):
            return ro + rva - va
    return None

if __name__ == '__main__':
    d, secs = load(sys.argv[1])
    for a in sys.argv[2:]:
        rva = int(a, 16)
        chain = [rva]
        for _ in range(4):
            o = off(secs, rva)
            if o is None or d[o] != 0xe9:
                break
            rva = (rva + 5 + struct.unpack_from('<i', d, o + 1)[0]) & 0xffffffff
            chain.append(rva)
        o = off(secs, rva)
        print(' -> '.join('%x' % c for c in chain), ':', d[o:o + 24].hex(' ') if o is not None else '?')
