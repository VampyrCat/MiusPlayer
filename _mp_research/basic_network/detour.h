// In-memory detour of an engine function: its first `len` bytes (whole instructions, none of them
// position dependent, at least 14) are replaced by a jump to `hook`; the returned trampoline runs those
// bytes and continues in the original, so the hook can still call it. Nothing on disk changes, and the
// caller checks the function's bytes against a known signature first, so an unknown build is left alone.
#pragma once
#include <windows.h>
#include <string.h>

inline void *install_detour(void *target, size_t len, void *hook)
{
    if (len < 14 || len > 32) return 0;
    unsigned char *fn = (unsigned char *)target;
    unsigned char *tramp = (unsigned char *)VirtualAlloc(0, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return 0;
    static const unsigned char jmp[6] = { 0xff, 0x25, 0, 0, 0, 0 };     // jmp [rip+0], address follows
    void *back = fn + len;
    memcpy(tramp, fn, len);
    memcpy(tramp + len, jmp, 6);
    memcpy(tramp + len + 6, &back, 8);
    DWORD old;
    if (!VirtualProtect(fn, len, PAGE_EXECUTE_READWRITE, &old)) { VirtualFree(tramp, 0, MEM_RELEASE); return 0; }
    unsigned char patch[32];
    memset(patch, 0x90, sizeof patch);
    memcpy(patch, jmp, 6);
    memcpy(patch + 6, &hook, 8);
    memcpy(fn, patch, len);
    VirtualProtect(fn, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), fn, len);
    FlushInstructionCache(GetCurrentProcess(), tramp, 64);
    return tramp;
}
