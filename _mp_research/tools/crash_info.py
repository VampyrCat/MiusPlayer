"""Summarises the engine's crash dump (out\\minidump.dmp): exception, faulting module and a rough call stack.
   crash_info.py [path to .dmp]"""
import os
import struct
import sys

path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'out', 'minidump.dmp')
d = open(path, 'rb').read()
sig, ver, nstreams, dirrva = struct.unpack_from('<IIII', d, 0)
streams = [struct.unpack_from('<III', d, dirrva + i * 12) for i in range(nstreams)]
mods, exc, ctx = [], None, None
for kind, size, rva in streams:
    if kind == 4:       # module list
        for j in range(struct.unpack_from('<I', d, rva)[0]):
            o = rva + 4 + j * 108
            base, sz = struct.unpack_from('<QI', d, o)
            namerva = struct.unpack_from('<I', d, o + 20)[0]
            ln = struct.unpack_from('<I', d, namerva)[0]
            name = d[namerva + 4:namerva + 4 + ln].decode('utf-16le').replace('/', chr(92)).split(chr(92))[-1]
            mods.append((base, sz, name))
    if kind == 6:       # exception
        code, flags, rec, addr, nparams = struct.unpack_from('<IIQQI', d, rva + 8)
        params = struct.unpack_from('<4Q', d, rva + 8 + 32)
        ctx = struct.unpack_from('<II', d, rva + 8 + 152)[1]
        exc = (code, addr, params[:nparams])


def where(a):
    for b, s, n in mods:
        if b <= a < b + s:
            return '%s+0x%x' % (n, a - b)
    return hex(a)


print('exception %#x at %s, parameters %s' % (exc[0], where(exc[1]), [hex(p) for p in exc[2]]))
regs = struct.unpack_from('<16Q', d, ctx + 0x78)       # rax rcx rdx rbx rsp rbp rsi rdi r8..r15
names = 'rax rcx rdx rbx rsp rbp rsi rdi r8 r9 r10 r11 r12 r13 r14 r15'.split()
print(' '.join('%s=%x' % (n, v) for n, v in zip(names, regs)))
rsp = regs[4]
boring = ('ntdll', 'KERNEL', 'ucrtbase', 'VCRUNTIME', 'MSVCP')
for kind, size, rva in streams:
    if kind == 3:       # thread list, with stack memory
        for j in range(struct.unpack_from('<I', d, rva)[0]):
            o = rva + 4 + j * 48
            sstart, ssize, srva = struct.unpack_from('<QII', d, o + 24)
            if sstart <= rsp < sstart + ssize:
                frames = []
                for k in range((rsp - sstart) // 8 * 8, min(ssize, (rsp - sstart) + 0x900), 8):
                    w = where(struct.unpack_from('<Q', d, srva + k)[0])
                    if '+' in w and not w.startswith(boring):
                        frames.append(w)
                print('stack (code addresses found, innermost first):')
                print('  ' + '\n  '.join(frames[:26]))
