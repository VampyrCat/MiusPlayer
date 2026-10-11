"""Read memory of a running game process: peek.py <pid> <hexaddr> [qwords]  -> values with module+rva."""
import ctypes, ctypes.wintypes as wt, sys, struct, os
k = ctypes.WinDLL('kernel32', use_last_error=True)
ps = ctypes.WinDLL('psapi', use_last_error=True)
k.OpenProcess.restype = wt.HANDLE
k.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
ps.EnumProcessModules.argtypes = [wt.HANDLE, ctypes.c_void_p, wt.DWORD, ctypes.POINTER(wt.DWORD)]
ps.GetModuleFileNameExA.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_char_p, wt.DWORD]
class MI(ctypes.Structure):
    _fields_ = [('base', ctypes.c_void_p), ('size', wt.DWORD), ('entry', ctypes.c_void_p)]
ps.GetModuleInformation.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.POINTER(MI), wt.DWORD]

def open_proc(pid):
    return k.OpenProcess(0x0410, False, pid)

def modules(h):
    arr = (ctypes.c_void_p * 1024)(); need = wt.DWORD()
    ps.EnumProcessModules(h, arr, ctypes.sizeof(arr), ctypes.byref(need))
    out = []
    for m in arr[:need.value // 8]:
        buf = ctypes.create_string_buffer(260); ps.GetModuleFileNameExA(h, m, buf, 260)
        mi = MI(); ps.GetModuleInformation(h, m, ctypes.byref(mi), ctypes.sizeof(mi))
        out.append((mi.base, mi.size, os.path.basename(buf.value.decode())))
    return out

def read(h, addr, n):
    buf = ctypes.create_string_buffer(n); got = ctypes.c_size_t()
    if not k.ReadProcessMemory(h, addr, buf, n, ctypes.byref(got)): return None
    return buf.raw

def where(mods, a):
    for b, s, n in mods:
        if b <= a < b + s: return '%s+0x%x' % (n, a - b)
    return ''

if __name__ == '__main__':
    pid = int(sys.argv[1]); addr = int(sys.argv[2], 16); n = int(sys.argv[3]) if len(sys.argv) > 3 else 8
    h = open_proc(pid); mods = modules(h)
    d = read(h, addr, n * 8)
    for i in range(n):
        v = struct.unpack_from('<Q', d, i * 8)[0]
        print('%x +0x%03x: %016x %s' % (addr, i * 8, v, where(mods, v)))
