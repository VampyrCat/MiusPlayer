"""Write memory of a running game process (experiments only)."""
import ctypes, ctypes.wintypes as wt
k = ctypes.WinDLL('kernel32', use_last_error=True)
k.OpenProcess.restype = wt.HANDLE
k.WriteProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
def open_rw(pid): return k.OpenProcess(0x0438, False, pid)
def write(h, addr, data):
    n = ctypes.c_size_t()
    return bool(k.WriteProcessMemory(h, addr, data, len(data), ctypes.byref(n)))
