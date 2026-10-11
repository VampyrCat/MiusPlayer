// basic_network.codelib -- multiplayer module for GRAVITECH3 v6.00 (GTMF build 3598).
// Fills the "i_network" slot the engine reserves. Stage 2: proper command table, per-frame tick,
// and a survey of every engine object (commands + handler addresses) written to out\mp_objects_<program>.txt.
#include "common.h"
#include <intrin.h>
#include <malloc.h>
#include <share.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------- logging
static FILE *g_log;
static CRITICAL_SECTION g_cs;
char g_out_dir[MAX_PATH];
char g_program[32] = "opgame";

static void log_open(HMODULE self)
{
    GetModuleFileNameA(self, g_out_dir, MAX_PATH);
    // <root>\bin\rel\x64\basic_network.codelib -> <root>\out
    for (int i = 0; i < 4; i++) {
        char *s = strrchr(g_out_dir, '\\');
        if (!s) break;
        *s = 0;
    }
    strcat_s(g_out_dir, "\\out");
    // one log per engine program: the operational and tactical modes are separate processes
    const char *prog = strstr(GetCommandLineA(), "root\\programs\\");
    if (prog) {
        prog += 14;
        size_t n = strcspn(prog, ".,\" ");
        if (n && n < sizeof g_program) { memcpy(g_program, prog, n); g_program[n] = 0; }
    }
    char path[MAX_PATH];
    sprintf_s(path, "%s\\basic_network_%s_%lu.log", g_out_dir, g_program, GetCurrentProcessId());
    InitializeCriticalSection(&g_cs);
    g_log = _fsopen(path, "w", _SH_DENYNO);
}

void mp_log(const char *fmt, ...)
{
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    EnterCriticalSection(&g_cs);
    fprintf(g_log, "%02u:%02u:%02u.%03u [t%05lu] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentThreadId());
    va_list a;
    va_start(a, fmt);
    vfprintf(g_log, fmt, a);
    va_end(a);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_cs);
}

bool readable(const void *p, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || !VirtualQuery(p, &mbi, sizeof mbi)) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    return (const char *)p + n <= (const char *)mbi.BaseAddress + mbi.RegionSize;
}

static const char *safe_str(const void *p, char *buf, size_t n)
{
    if (!readable(p, 1)) return "<bad ptr>";
    size_t i = 0;
    const unsigned char *s = (const unsigned char *)p;
    for (; i + 1 < n && readable(s + i, 1) && s[i]; i++) {
        if (s[i] < 0x20 || s[i] > 0x7e) return "<not text>";
        buf[i] = (char)s[i];
    }
    buf[i] = 0;
    return buf;
}

static void log_hex(const char *what, const void *p, size_t n)
{
    char line[3 * 64 + 1];
    if (!readable(p, n)) { mp_log("%s: %p <unreadable>", what, p); return; }
    for (size_t o = 0; o < n; o += 32) {
        size_t k = n - o < 32 ? n - o : 32, w = 0;
        for (size_t i = 0; i < k; i++)
            w += sprintf_s(line + w, sizeof line - w, "%02x%s", ((const unsigned char *)p)[o + i], (i & 7) == 7 ? " " : "");
        mp_log("%s +0x%02zx: %s", what, o, line);
    }
}

// "adv_game.codelib+0x1234" for an address inside a loaded module
static const char *where(const void *addr, char *buf, size_t n)
{
    HMODULE m = 0;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &m) || !m) {
        sprintf_s(buf, n, "%p", addr);
        return buf;
    }
    char path[MAX_PATH];
    GetModuleFileNameA(m, path, MAX_PATH);
    const char *base = strrchr(path, '\\');
    sprintf_s(buf, n, "%s+0x%llx", base ? base + 1 : path, (u64)((const char *)addr - (const char *)m));
    return buf;
}

// Switches, set from the environment for now (a network session will set g_lockstep itself):
//   MP_LOCKSTEP=1  fixed frame time and locked game speed, as needed for step-identical simulation
//   MP_DEBUG=1     object survey, per-second unit fingerprints, random-number probe
static bool g_lockstep, g_debug;
static const u64 k_start_frame = 4500;

// ---------------------------------------------------------------- object survey
static const struct { const char *iface, *name; } k_objects[] = {
#include "objects.inc"
};

static void survey(ObjMan *om)
{
    char path[MAX_PATH], b1[96], b2[96], b3[64];
    sprintf_s(path, "%s\\mp_objects_%s.txt", g_out_dir, g_program);
    FILE *f = _fsopen(path, "w", _SH_DENYNO);
    if (!f) return;
    int found = 0;
    for (const auto &k : k_objects) {
        Object *o = 0;
        try { o = engine_find(om, k.iface, k.name); } catch (...) { o = 0; }
        if (!o || !readable(o, sizeof(Object))) continue;
        found++;
        fprintf(f, "%s:%s  obj=%p  vtbl=%s  refs=%u flags=%u\n", k.iface, k.name, o, where(o->vtbl, b1, sizeof b1), o->refs, o->flags);
        if (readable(o->vtbl, 24))
            fprintf(f, "  dispatch=%s\n", where(o->vtbl[2], b1, sizeof b1));
        if (o->dep_n < 256 && readable(o->deps, o->dep_n * sizeof(Dep))) {
            fprintf(f, "  deps:");
            for (u32 i = 0; i < o->dep_n; i++)
                fprintf(f, " %s:%s", safe_str(o->deps[i].iface, b2, sizeof b2), safe_str(o->deps[i].name, b3, sizeof b3));
            fprintf(f, "\n");
        }
        CmdTable &t = o->cmds;
        if (t.count < 4096 && readable(t.vals, t.count * 8) && readable(t.keys, t.count * 8))
            for (u32 i = 0; i < t.count; i++)
                fprintf(f, "  cmd %-28s %s\n", safe_str(t.keys[i], b2, sizeof b2), where((void *)t.vals[i], b1, sizeof b1));
    }
    fclose(f);
    mp_log("object survey: %d objects written to %s", found, path);
}

// ---------------------------------------------------------------- random-number probe
// Every module draws from the CRT rand(). Routing those calls through here shows how much
// randomness each simulation step consumes, which is the cheapest fingerprint of "did two
// instances compute the same step".
static int (*g_real_rand)(void);
static void (*g_real_srand)(unsigned);
static volatile LONG64 g_rand_calls, g_rand_sum;
static DWORD g_rand_threads[32];
static volatile LONG g_rand_nthreads;

static void note_thread(void)
{
    DWORD id = GetCurrentThreadId();
    LONG n = g_rand_nthreads;
    for (LONG i = 0; i < n; i++)
        if (g_rand_threads[i] == id) return;
    EnterCriticalSection(&g_cs);
    if (g_rand_nthreads < 32) g_rand_threads[g_rand_nthreads++] = id;
    LeaveCriticalSection(&g_cs);
}

static int hook_rand(void)
{
    int v = g_real_rand();
    InterlockedIncrement64(&g_rand_calls);
    InterlockedAdd64(&g_rand_sum, v);
    note_thread();
    return v;
}

static void hook_srand(unsigned seed)
{
    char b[96];
    mp_log("srand(%u) from %s", seed, where(_ReturnAddress(), b, sizeof b));
    g_real_srand(seed);
}

static int patch_imports(HMODULE mod, void *from, void *to)
{
    BYTE *base = (BYTE *)mod;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return 0;
    int n = 0;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); d->Name; d++)
        for (void **t = (void **)(base + d->FirstThunk); *t; t++)
            if (*t == from) {
                DWORD old;
                VirtualProtect(t, sizeof *t, PAGE_READWRITE, &old);
                *t = to;
                VirtualProtect(t, sizeof *t, old, &old);
                n++;
            }
    return n;
}

static void hook_random(void)
{
    static const char *mods[] = {
        "adv_game.codelib", "adv_interf.codelib", "adv_opgame.codelib", "adv_physic.codelib", "adv_qbattle.codelib",
        "adv_render.codelib", "base_main.codelib", "base_shell.codelib", "basic_game.codelib", "basic_input.codelib",
        "basic_physic.codelib", "basic_render.codelib", "basic_sound.codelib",
    };
    HMODULE crt = GetModuleHandleA("ucrtbase.dll");
    if (!crt || g_real_rand) return;
    g_real_rand = (int (*)(void))GetProcAddress(crt, "rand");
    g_real_srand = (void (*)(unsigned))GetProcAddress(crt, "srand");
    for (const char *m : mods)
        if (HMODULE h = GetModuleHandleA(m)) {
            int a = patch_imports(h, (void *)g_real_rand, (void *)hook_rand);
            int b = patch_imports(h, (void *)g_real_srand, (void *)hook_srand);
            if (a || b) mp_log("random probe: %s rand x%d srand x%d", m, a, b);
        }
}

// ---------------------------------------------------------------- keep running without focus
// The interface pauses the battle when the window is deactivated. In a network game the other side
// keeps playing, so deactivation is hidden from the engine.
static WNDPROC g_prev_wndproc;

static LRESULT CALLBACK focus_wndproc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    if ((m == WM_ACTIVATEAPP && !wp) || (m == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE) || m == WM_KILLFOCUS)
        return 0;
    if (ui_blocks_input() && ((m >= WM_MOUSEFIRST && m <= WM_MOUSELAST) || (m >= WM_KEYFIRST && m <= WM_KEYLAST)))
        return 0;
    return CallWindowProcA(g_prev_wndproc, w, m, wp, lp);
}

static BOOL CALLBACK find_main_window(HWND w, LPARAM out)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(w) || GetWindow(w, GW_OWNER)) return TRUE;
    *(HWND *)out = w;
    return FALSE;
}

HWND main_window(void)
{
    HWND w = 0;
    EnumWindows(find_main_window, (LPARAM)&w);
    return w;
}

static void keep_running_unfocused(ObjMan *om)
{
    HWND w = 0;
    if (g_prev_wndproc) return;
    // The engine's own "work without focus" switch: i_loader +0x2ac is the setting, i_window vtbl[8] applies it.
    // Without it an inactive window only gets the reduced "napump" frame.
    Object *loader = engine_find(om, "i_loader", "loader"), *window = engine_find(om, "i_window", "window");
    if (loader) ((char *)loader)[0x2ac] = 1;
    if (window) ((void (*)(Object *, char))window->vtbl[8])(window, 1);
    EnumWindows(find_main_window, (LPARAM)&w);
    if (!w) return;
    g_prev_wndproc = (WNDPROC)SetWindowLongPtrA(w, GWLP_WNDPROC, (LONG_PTR)focus_wndproc);
    mp_log("window %p: deactivation hidden from the engine", w);
    if (GetForegroundWindow() != w) {
        // already in the background (e.g. a second instance started on top): tell the engine it is active
        CallWindowProcA(g_prev_wndproc, w, WM_ACTIVATEAPP, TRUE, 0);
        CallWindowProcA(g_prev_wndproc, w, WM_ACTIVATE, WA_ACTIVE, 0);
        CallWindowProcA(g_prev_wndproc, w, WM_SETFOCUS, 0, 0);
    }
}

// ---------------------------------------------------------------- test helpers (environment-driven)
// MP_WINDOW=x,y,w,h places the game window, so two instances can sit side by side on one screen.
static void place_window_from_env(void)
{
    char v[64];
    int x, y, w, h;
    HWND win = 0;
    if (!GetEnvironmentVariableA("MP_WINDOW", v, sizeof v) || sscanf_s(v, "%d,%d,%d,%d", &x, &y, &w, &h) != 4) return;
    EnumWindows(find_main_window, (LPARAM)&win);
    if (win && SetWindowPos(win, HWND_TOP, x, y, w, h, SWP_NOACTIVATE | SWP_NOZORDER))
        mp_log("window placed at %d,%d %dx%d", x, y, w, h);
}

// MP_PROFILE=1 samples the main thread twice a second and logs where it is, to find what stalls a frame.
static DWORD WINAPI profile_thread(LPVOID main_thread)
{
    char b[96];
    for (int i = 0; i < 240; i++) {
        Sleep(500);
        CONTEXT c;
        c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        if (SuspendThread((HANDLE)main_thread) == (DWORD)-1) break;
        BOOL ok = GetThreadContext((HANDLE)main_thread, &c);
        u64 stack[48] = {0};
        if (ok) memcpy(stack, (void *)c.Rsp, readable((void *)c.Rsp, sizeof stack) ? sizeof stack : 0);
        ResumeThread((HANDLE)main_thread);
        if (!ok) continue;
        char line[1024];
        size_t w = sprintf_s(line, "profile: rip=%s stack:", where((void *)c.Rip, b, sizeof b));
        int shown = 0;
        for (u64 s : stack) {
            HMODULE m = 0;
            if (shown < 7 && s > 0x10000 && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)s, &m) && m) {
                w += sprintf_s(line + w, sizeof line - w, " %s", where((void *)s, b, sizeof b));
                shown++;
            }
        }
        mp_log("%s", line);
    }
    return 0;
}

static void start_profiler_from_env(void)
{
    char v[8];
    if (!GetEnvironmentVariableA("MP_PROFILE", v, sizeof v)) return;
    HANDLE main_thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, GetCurrentThreadId());
    if (main_thread) CloseHandle(CreateThread(0, 0, profile_thread, main_thread, 0, 0));
}

// ---------------------------------------------------------------- input isolation for the multiplayer menu
// While the menu is open the game must not react to the mouse or keyboard. The engine reads them through
// raw input and key-state polling, so those calls are routed through here and blanked while the menu is up.
static UINT (WINAPI *g_real_GetRawInputData)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
static SHORT (WINAPI *g_real_GetAsyncKeyState)(int);
static SHORT (WINAPI *g_real_GetKeyState)(int);
static BOOL (WINAPI *g_real_GetKeyboardState)(PBYTE);

static UINT WINAPI hook_GetRawInputData(HRAWINPUT h, UINT cmd, LPVOID data, PUINT size, UINT header)
{
    UINT r = g_real_GetRawInputData(h, cmd, data, size, header);
    if (cmd == RID_INPUT && data && r != (UINT)-1 && r >= sizeof(RAWINPUTHEADER) && ui_blocks_input()) {
        RAWINPUT *in = (RAWINPUT *)data;
        if (in->header.dwType == RIM_TYPEMOUSE) {
            in->data.mouse.usButtonFlags = 0;
            in->data.mouse.usButtonData = 0;
        } else if (in->header.dwType == RIM_TYPEKEYBOARD) {
            in->data.keyboard.VKey = 0xff;
            in->data.keyboard.MakeCode = 0;
            in->data.keyboard.Flags = RI_KEY_BREAK;
            in->data.keyboard.Message = WM_KEYUP;
        }
    }
    return r;
}

static SHORT WINAPI hook_GetAsyncKeyState(int vk) { return ui_blocks_input() ? 0 : g_real_GetAsyncKeyState(vk); }
static SHORT WINAPI hook_GetKeyState(int vk) { return ui_blocks_input() ? 0 : g_real_GetKeyState(vk); }

static BOOL WINAPI hook_GetKeyboardState(PBYTE state)
{
    BOOL r = g_real_GetKeyboardState(state);
    if (r && state && ui_blocks_input()) memset(state, 0, 256);
    return r;
}

static void isolate_input(void)
{
    static const char *mods[] = {
        "adv_game.codelib", "adv_interf.codelib", "adv_opgame.codelib", "adv_qbattle.codelib", "base_main.codelib",
        "base_shell.codelib", "basic_game.codelib", "basic_input.codelib", "base_encycl.codelib",
    };
    HMODULE user = GetModuleHandleA("user32.dll");
    if (!user || g_real_GetRawInputData) return;
    g_real_GetRawInputData = (UINT (WINAPI *)(HRAWINPUT, UINT, LPVOID, PUINT, UINT))GetProcAddress(user, "GetRawInputData");
    g_real_GetAsyncKeyState = (SHORT (WINAPI *)(int))GetProcAddress(user, "GetAsyncKeyState");
    g_real_GetKeyState = (SHORT (WINAPI *)(int))GetProcAddress(user, "GetKeyState");
    g_real_GetKeyboardState = (BOOL (WINAPI *)(PBYTE))GetProcAddress(user, "GetKeyboardState");
    int n = 0;
    for (const char *m : mods)
        if (HMODULE h = GetModuleHandleA(m)) {
            n += patch_imports(h, (void *)g_real_GetRawInputData, (void *)hook_GetRawInputData);
            n += patch_imports(h, (void *)g_real_GetAsyncKeyState, (void *)hook_GetAsyncKeyState);
            n += patch_imports(h, (void *)g_real_GetKeyState, (void *)hook_GetKeyState);
            n += patch_imports(h, (void *)g_real_GetKeyboardState, (void *)hook_GetKeyboardState);
        }
    mp_log("input isolation ready (%d entry points)", n);
}

// ---------------------------------------------------------------- frame pacing between game instances
// A game that is not the foreground window keeps simulating but does not need to render flat out; it is held
// to MP_BG_FPS frames per second (default 30, 0 = no cap).
// When several instances run on one PC (a host and a joining game side by side) the one in front would
// still take the whole graphics card at 100 frames per second and starve the other down to a few frames per
// second, which stalls its simulation. The instances therefore publish their frame rate in a small shared
// memory block, and the one in front lowers its own rate until the others reach theirs.
// MP_FG_FPS pins the rate of the game in front instead (0 = never limit it).
struct PaceSlot { DWORD pid, when; float fps; DWORD front; };
static const int k_pace_slots = 8;

static void frame_pacing(void)
{
    static int bg_cap = -1, fg_fixed = -1;
    static double front_cap = 60;
    static LARGE_INTEGER freq, last;
    static PaceSlot *slots, *mine;
    static DWORD window_start, frames, others;
    static float slowest_other = 1000;
    if (bg_cap < 0) {
        char v[16];
        bg_cap = GetEnvironmentVariableA("MP_BG_FPS", v, sizeof v) ? atoi(v) : 30;
        if (GetEnvironmentVariableA("MP_FG_FPS", v, sizeof v)) fg_fixed = atoi(v);
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&last);
        HANDLE map = CreateFileMappingA(INVALID_HANDLE_VALUE, 0, PAGE_READWRITE, 0, sizeof(PaceSlot) * k_pace_slots, "Local\\gtmf_mp_frame_pacing");
        if (map) slots = (PaceSlot *)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, 0);
        window_start = GetTickCount();
    }
    HWND w = main_window();
    bool front = w && GetForegroundWindow() == w;
    DWORD tick = GetTickCount();
    frames++;
    if (tick - window_start >= 500) {
        float fps = frames * 1000.0f / (tick - window_start);
        frames = 0;
        window_start = tick;
        others = 0;
        slowest_other = 1000;
        if (slots) {
            DWORD pid = GetCurrentProcessId();
            if (!mine || mine->pid != pid) {
                mine = 0;
                for (int i = 0; i < k_pace_slots && !mine; i++)
                    if (slots[i].pid == pid || !slots[i].pid || tick - slots[i].when > 5000) mine = &slots[i];
            }
            if (mine) { mine->pid = pid; mine->when = tick; mine->fps = fps; mine->front = front; }
            for (int i = 0; i < k_pace_slots; i++) {
                if (&slots[i] == mine || !slots[i].pid || tick - slots[i].when > 2000) continue;
                others++;
                if (slots[i].fps < slowest_other) slowest_other = slots[i].fps;
            }
        }
        // in front, with other instances on this PC: give way until the slowest of them is near its own cap
        float enough = bg_cap > 0 ? bg_cap * 0.9f : 27;
        if (slowest_other < enough) front_cap = front_cap > 36 ? front_cap * 0.85 : 30;
        else front_cap = front_cap < 96 ? front_cap + 2 : 100;
    }
    double cap = 0;
    if (!front) cap = bg_cap;
    else if (fg_fixed >= 0) cap = fg_fixed;
    else if (others) cap = front_cap;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (cap > 0) {
        double elapsed = (double)(now.QuadPart - last.QuadPart) / freq.QuadPart, want = 1.0 / cap;
        if (elapsed < want) {
            Sleep((DWORD)((want - elapsed) * 1000));
            QueryPerformanceCounter(&now);
        }
    }
    last = now;
}

// ---------------------------------------------------------------- the network object
struct Network : Object {
    Handler     val_store[32];
    const char *key_store[32];
    HashSlot    slot_store[64];
    u32         cfg_port, cfg_clients;
    u64         cfg_delay_init, cfg_direct;
    u64         ticks, generates, ctrl_ticks, sim_ms;
    Object     *ctrltimer;
    DWORD       tick_t0;
    bool        surveyed;
    FILE       *trace;
};

static void add_cmd(Network *n, const char *name, Handler fn)
{
    CmdTable &t = n->cmds;
    u32 idx = t.count++;
    t.key_count = t.count;
    t.keys[idx] = name;
    t.vals[idx] = fn;
    u32 h = t.hash(&name, t.nslots);
    for (u32 i = 0; i < t.nslots; i++) {
        HashSlot &s = t.slots[(i + h) & (t.nslots - 1)];
        if (!(s.hash & 0x80000000u)) { s.hash = h | 0x80000000u; s.index = idx; break; }
    }
}

// "sync": the engine asks every object to resolve its dependencies, then marks it ready
static void on_sync(Object *o, void *)
{
    o->flags &= ~1u;
    for (u32 i = 0; i < o->dep_n; i++) {
        Dep &d = o->deps[i];
        *d.dest = ((Object *(*)(ObjMan *, Dep *))o->objman->vtbl[9])(o->objman, &d);
    }
    o->flags |= 1;
    mp_log("sync: objman=%p", o->objman);
    if (g_debug) hook_random();
}

// "ctrl_tick": simulation step from the p_ctrl pipeline, before every other object.
// Message: +0x10 float scaled frame time, +0x14 float step seconds, +0x18 u32 step ms, +0x1c flags (1 first, 2 last, 4 paused)
static void on_ctrl_tick(Object *o, void *msg)
{
    Network *n = (Network *)o;
    if (!n->ctrl_ticks) {
        mp_log("first ctrl_tick at frame %llu (msg=%p)", n->ticks, msg);
        char path[MAX_PATH];
        sprintf_s(path, "%s\\mp_trace_%s_%lu.txt", g_out_dir, g_program, GetCurrentProcessId());
        if (g_debug) n->trace = _fsopen(path, "w", _SH_DENYNO);
    }
    u32 ms = *(u32 *)((char *)msg + 0x18), fl = *(u32 *)((char *)msg + 0x1c);
    session_note_sim_ms(ms);
    session_sim_step(o->objman);
    if (g_debug) state_debug_step(o->objman, n->sim_ms);
    n->ctrl_ticks++;
    u64 before = n->sim_ms;
    n->sim_ms += ms;
    // MP_DUMP=1: full unit positions at a few fixed moments, to see which units differ between instances
    if (ms && (n->sim_ms == 10 || n->sim_ms == 1000 || n->sim_ms == 5000 || n->sim_ms == 20000) &&
        GetEnvironmentVariableA("MP_DUMP", 0, 0)) {
        char path[MAX_PATH];
        sprintf_s(path, "%s\\mp_units_%lu_%llu.txt", g_out_dir, GetCurrentProcessId(), n->sim_ms);
        if (FILE *f = _fsopen(path, "w", _SH_DENYNO)) {
            Object **all = *(Object ***)((char *)o->objman + 0xf0);
            u32 count = *(u32 *)((char *)o->objman + 0xfc);
            for (u32 i = 0; i < count; i++) {
                Object *u = all[i];
                if (strcmp(u->iface, "i_techn") && strcmp(u->iface, "i_human")) continue;
                float *p = (float *)((char *)u + 0x1b0);
                fprintf(f, "%s %.6f %.6f %.6f\n", u->full, p[0], p[1], p[2]);
            }
            fclose(f);
        }
    }
    // once per simulated second: fingerprint of every unit position, to compare instances step for step
    if (n->trace && ms && before / 1000 != n->sim_ms / 1000) {
        ObjMan *om = o->objman;
        Object **all = *(Object ***)((char *)om + 0xf0);
        u32 count = *(u32 *)((char *)om + 0xfc), units = 0;
        u64 hash = 1469598103934665603ull;
        float first[3] = {0, 0, 0};
        for (u32 i = 0; i < count; i++) {
            Object *u = all[i];
            if (strcmp(u->iface, "i_techn") && strcmp(u->iface, "i_human")) continue;
            const unsigned char *pos = (const unsigned char *)u + 0x1b0;   // world position x, height, z
            if (!units++) memcpy(first, pos, 12);
            for (int b = 0; b < 12; b++) hash = (hash ^ pos[b]) * 1099511628211ull;
        }
        fprintf(n->trace, "sim=%llu step=%llu frame=%llu ms=%u fl=%u dt=%g rng=%08x units=%u hash=%016llx first=%.3f,%.3f,%.3f\n",
                n->sim_ms, n->ctrl_ticks, n->ticks, ms, fl, *(float *)((char *)msg + 0x10), *(u32 *)((char *)om + 0xac8),
                units, hash, first[0], first[1], first[2]);
        fflush(n->trace);
    }
}

// "tick": once per rendered frame from the p_main pipeline
static void on_tick(Object *o, void *msg)
{
    Network *n = (Network *)o;
    if (!n->ticks++) {
        n->tick_t0 = GetTickCount();
        mp_log("first tick (msg=%p)", msg);
        keep_running_unfocused(o->objman);
        place_window_from_env();
        start_profiler_from_env();
        isolate_input();
        session_start(o->objman);
        ui_start(o->objman);
    }
    session_frame(o->objman);
    ui_frame(o->objman);
    frame_pacing();
    // Lockstep only: lock the simulation clock (no automatic speed adjustment, speed x1). With the fixed
    // frame time this makes every frame advance the battle by exactly the same step on every machine.
    // Never done in ordinary single-player use, where the player's speed buttons must keep working.
    //   i_ctrltimer: +0x108 requested speed, +0x10c current speed, +0x15c pause, +0x15d automatic speed control
    if (g_lockstep && !n->ctrltimer) n->ctrltimer = engine_find(o->objman, "i_ctrltimer", "ctrltimer");
    if (g_lockstep && n->ctrltimer) {
        char *ct = (char *)n->ctrltimer;
        ct[0x15d] = 0;
        *(float *)(ct + 0x108) = 1.0f;
        *(float *)(ct + 0x10c) = 1.0f;
        // Experiment: start the simulation on the same frame number in every instance. The engine lifts its
        // opening pause after a varying number of frames; hold the pause until a fixed frame instead.
        if (n->ticks < k_start_frame) ct[0x15c] = 1;
        else if (n->ticks == k_start_frame) { ct[0x15c] = 0; mp_log("simulation released at frame %llu", n->ticks); }
    }
    if (g_debug && !n->surveyed && n->ticks == 120) {
        n->surveyed = true;
        survey(o->objman);
    }
    if (n->ticks % 3000 == 0)
        mp_log("tick %llu (%.1f per second)", n->ticks, n->ticks * 1000.0 / (GetTickCount() - n->tick_t0 + 1));
}

// "render": from the r_main pipeline, placed just below the game's cursor (see tools\make_progpack.py)
static void on_render(Object *o, void *)
{
    static bool logged;
    if (!logged) { logged = true; mp_log("render pipeline reached the network object"); }
    ui_render(o->objman);
}

// "generate": once per loading step from the g_main pipeline
static void on_generate(Object *o, void *msg)
{
    Network *n = (Network *)o;
    if (!n->generates++) {
        mp_log("first generate (msg=%p)", msg);
        // Fixed frame time: every rendered frame then advances the simulation by the same amount on every
        // machine (i_timer vtbl[3] = set_fixed_tick). 10.5 ms keeps the 10 ms quantisation stable.
        Object *timer = g_lockstep ? engine_find(o->objman, "i_timer", "timer") : 0;
        if (timer) {
            ((void (*)(Object *, float))timer->vtbl[3])(timer, 0.0105f);
            mp_log("fixed frame time set on %p", timer);
        }
    }
}

struct Seen { char name[48]; u64 count; };
static Seen g_seen[64];
static int g_nseen;

static void *net_dtor(Network *n, u64 flags)
{
    mp_log("destroy '%s' flags=%llu, ticks=%llu generates=%llu ctrl_ticks=%llu rand=%lld", n->full, flags, n->ticks,
         n->generates, n->ctrl_ticks, (long long)g_rand_calls);
    if (n->trace) fclose(n->trace);
    session_stop();
    for (int i = 0; i < g_nseen; i++) mp_log("  unhandled command '%s' received %llu times", g_seen[i].name, g_seen[i].count);
    if ((flags & 1) && !(flags & 4)) _aligned_free(n);
    return n;
}

static u64 net_query(Network *, u32 *out)
{
    if (out) *out = 0;
    return 0;
}

static void net_dispatch(Network *n, Msg *msg)
{
    char buf[48];
    const char *name = "<no msg>";
    if (readable(msg, 16)) name = safe_str(msg->name, buf, sizeof buf);
    if (Handler h = engine_lookup(n, name)) { h(n, msg); return; }
    if (!strcmp(name, "gamemode") && readable(msg, 0x18)) session_note_phase(*(u32 *)((char *)msg + 0x10));
    if (g_debug && (!strcmp(name, "gamemode") || !strcmp(name, "actmode"))) {
        // battle phase changes (deployment, battle, statistics) arrive as these broadcasts
        static int shown;
        if (shown++ < 60) {
            mp_log("%s at frame %llu sim %llu ms", name, n->ticks, n->sim_ms);
            log_hex("  msg", msg, 0x40);
        }
    }
    for (int i = 0; i < g_nseen; i++)
        if (!strcmp(g_seen[i].name, name)) { g_seen[i].count++; return; }
    if (g_nseen < 64) {
        strcpy_s(g_seen[g_nseen].name, name);
        g_seen[g_nseen++].count = 1;
    }
    mp_log("unhandled command '%s' (msg=%p)", name, msg);
    log_hex("  msg", msg, 0x40);
}

static u64 net_extra(void *o)
{
    mp_log("unexpected extra virtual call on %p", o);
    return 0;
}

static void *g_net_vtbl[8] = {
    (void *)net_dtor, (void *)net_query, (void *)net_dispatch,
    (void *)net_extra, (void *)net_extra, (void *)net_extra, (void *)net_extra, (void *)net_extra,
};

static Object *network_create(CreateCtx *ctx)
{
    Network *n = (Network *)_aligned_malloc(sizeof(Network), 0x10);
    if (!n) return 0;
    memset(n, 0, sizeof *n);
    n->vtbl = g_net_vtbl;
    sprintf_s(n->full, "i_network:%.31s", ctx->inst ? ctx->inst : "");
    n->iface = "i_network";
    n->inst = n->full + 10;
    n->author = "community";
    n->objman = ctx->objman;
    n->refs = 1;
    n->flags = 2;
    n->cmds.vals = n->val_store;
    n->cmds.keys = n->key_store;
    n->cmds.slots = n->slot_store;
    n->cmds.cap = n->cmds.key_cap = n->cmds.limit = 32;
    n->cmds.nslots = n->cmds.slot_cap = 64;
    n->cmds.hash = engine_hash;
    n->cmds.factor = 2;
    add_cmd(n, "sync", on_sync);
    add_cmd(n, "tick", on_tick);
    add_cmd(n, "generate", on_generate);
    add_cmd(n, "ctrl_tick", on_ctrl_tick);
    add_cmd(n, "render", on_render);

    void **reader = ctx->reader;
    u32 handle = 0;
    if (reader && ((char (*)(void *, const char *, u64, u32 *, u64))(*(void ***)reader)[2])(reader, n->full, 0, &handle, 0)) {
        Field f[] = {
            { "u:port", &n->cfg_port }, { "u:clients", &n->cfg_clients },
            { "b:delay_init", &n->cfg_delay_init }, { "b:direct_connect", &n->cfg_direct },
        };
        ((void (*)(void *, u32, Field *, u32))(*(void ***)reader)[6])(reader, handle, f, 4);
    }
    mp_log("created '%s' at %p: port=%u clients=%u delay_init=%u direct_connect=%u", n->full, n,
         n->cfg_port, n->cfg_clients, (u32)n->cfg_delay_init, (u32)n->cfg_direct);
    return n;
}

// ---------------------------------------------------------------- factory
// GetFactory3() returns an object with three virtuals:
//   [0] create(factory, class, ctx)   [1] register(factory, class, creator)   [2] enumerate(factory, index, name*)
struct Factory { void **vtbl; };

static void *fac_create(Factory *, const char *name, CreateCtx *ctx)
{
    if (name && !strcmp(name, "i_network")) return network_create(ctx);
    return 0;
}

static u64 fac_register(Factory *, const char *, void *) { return 1; }

static u32 fac_enum(Factory *, u32 index, const char **out)
{
    if (!out) return 1;
    if (index) return 0;
    *out = "i_network";
    return 1;
}

static void *g_fac_vtbl[4] = { (void *)fac_create, (void *)fac_register, (void *)fac_enum, (void *)net_extra };
static Factory g_factory = { g_fac_vtbl };

extern "C" __declspec(dllexport) void *GetFactory3(void)
{
    return &g_factory;
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(self);
        log_open(self);
        mp_log("basic_network.codelib loaded, program '%s', pid %lu", g_program, GetCurrentProcessId());
        mp_log("command line: %s", GetCommandLineA());
        g_lockstep = GetEnvironmentVariableA("MP_LOCKSTEP", 0, 0) != 0;
        g_debug = GetEnvironmentVariableA("MP_DEBUG", 0, 0) != 0;
        mp_log("lockstep %s, debug %s", g_lockstep ? "on" : "off", g_debug ? "on" : "off");
    } else if (reason == DLL_PROCESS_DETACH && g_log) {
        mp_log("unloaded");
    }
    return TRUE;
}
