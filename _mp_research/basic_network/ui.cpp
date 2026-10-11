// In-game multiplayer menu, lobby and status banner.
//
// The game's main menu gets one more entry, MULTIPLAYER, drawn in the menu's own style at the top of the
// list. It opens a panel:
//     HOST A SKIRMISH | HOST A CUSTOM MISSION | HOST AN OPERATION | JOIN A GAME (address entry)
// Hosting and joining both lead to the lobby: who is there, who commands which force, who is ready.
// The host sets the battle up on the game's own Skirmish / Missions / Operations screen; F9 brings the
// lobby back.
// Everything is drawn with the game's own Direct3D 9 device from inside its render pipeline, just below
// the game's cursor. Text is rasterised with GDI into managed textures, so nothing here needs
// device-reset handling.
#include "common.h"
#include "joiner.h"
#include <d3d9.h>
#include <ctype.h>
#include <math.h>
#include <string>
#include <unordered_map>
#include <vector>

enum Screen { SCR_NONE, SCR_MAIN, SCR_JOIN, SCR_LOBBY, SCR_STARTING, SCR_MAPWAIT };
static Screen g_screen = SCR_NONE;
static bool g_tactical, g_lobby_host, g_button_hot, g_on_main_menu, g_on_op_map, g_map_button_hot, g_map_busy;
static char g_autojoin[128];     // back from the operational map: rejoin this host's lobby, ready
static DWORD g_block_until, g_exit_at, g_frames;
static Joiner g_joiner;
static char g_address[64], g_root[MAX_PATH], g_message[200];
enum Action { ACT_NONE, ACT_OPEN, ACT_HOST_SKIRMISH, ACT_HOST_MISSION, ACT_HOST_OPERATION, ACT_CONNECT, ACT_LEAVE, ACT_SETUP, ACT_SWAP,
              ACT_READY, ACT_BACK, ACT_RESTORE, ACT_MAP_GIVE, ACT_MAP_CANCEL };
static Action g_action;      // chosen while drawing, carried out in the next logic pass (never inside the render pipeline)
static int g_action_arg;

// ---------------------------------------------------------------- input (polled, independent of the game's input)
static bool g_mouse_down, g_mouse_was_down, g_clicked;
static float g_mx, g_my;
static unsigned char g_key_was[256];

static bool key_pressed(int vk)
{
    bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool edge = down && !g_key_was[vk];
    g_key_was[vk] = down;
    return edge;
}

bool ui_blocks_input(void) { return g_screen != SCR_NONE || g_button_hot || g_map_button_hot || g_map_busy || GetTickCount() < g_block_until; }

static void close_menu(void)
{
    g_screen = SCR_NONE;
    g_block_until = GetTickCount() + 250;      // the click that closed the menu must not reach the game
    SetEnvironmentVariableA("MP_MENU", "done");
}

// ---------------------------------------------------------------- engine hooks: main menu state, screen switch
// i_opgame +0x100 -> menu controller; controller +0x1258 = current screen (0 main menu, 2 campaign, 0x11 skirmish).
// Screen changes go through adv_opgame+0x762e0(controller, screen, 0); the stock menu buttons are thin stubs
// around it. The stubs are checked byte for byte before use, so a different game build simply skips them.
static void *menu_controller(ObjMan *om)
{
    Object *og = engine_find(om, "i_opgame", "opgame");
    if (!og) return 0;
    void *c = *(void **)((char *)og + 0x100);
    return readable(c, 0x1260) ? c : 0;
}

static int menu_screen(ObjMan *om)
{
    void *c = menu_controller(om);
    return c ? *(int *)((char *)c + 0x1258) : -1;
}

// The main menu itself: screen 0 while the program is in its menu mode. The operational map of an
// operation is screen 0 as well; controller +0x1200 tells them apart (4 in the menus, 0 on the map).
static bool on_main_menu(ObjMan *om)
{
    void *c = menu_controller(om);
    return c && *(int *)((char *)c + 0x1258) == 0 && *(int *)((char *)c + 0x1200) != 0;
}

static void open_stock_screen(ObjMan *om, int mode)
{
    HMODULE m = GetModuleHandleA("adv_opgame.codelib");
    void *c = menu_controller(om);
    if (!m || !c) return;
    unsigned char *base = (unsigned char *)m;
    const char *name = mode == HOST_MODE_OPERATION ? "campaign" : mode == HOST_MODE_MISSION ? "missions" : "skirmish";
    if (mode == HOST_MODE_SKIRMISH) {
        static const unsigned char head[] = { 0x4c, 0x89, 0x44, 0x24, 0x18, 0x48, 0x89, 0x54, 0x24, 0x10, 0x53, 0x48, 0x83, 0xec, 0x40,
                                              0x83, 0xb9, 0x58, 0x12, 0x00, 0x00, 0x11 };                 // "do_mm_skirmish"
        if (memcmp(base + 0x627f0, head, sizeof head)) { mp_log("ui: unknown game build, open SKIRMISH yourself"); return; }
        ((void (*)(void *, u64, u64, char *))(base + 0x627f0))(c, 0, 0, 0);
    } else {
        // "do_mm_campaign" (screen 2) and "do_mm_qbattle" (screen 3, the MISSIONS entry: prepared and
        // user-made missions) are stubs of the same shape: xor r8d,r8d / mov edx,<screen> / jmp switch
        size_t at = mode == HOST_MODE_OPERATION ? 0x2b210 : 0x2b250;
        const unsigned char stub[] = { 0x45, 0x33, 0xc0, 0xba, (unsigned char)(mode == HOST_MODE_OPERATION ? 2 : 3), 0x00, 0x00, 0x00, 0xe9 };
        if (memcmp(base + at, stub, sizeof stub)) { mp_log("ui: unknown game build, open the %s screen yourself", name); return; }
        ((void (*)(void *))(base + at))(c);
    }
    mp_log("ui: opened the stock %s screen", name);
}

// ---------------------------------------------------------------- Direct3D device
// The engine keeps its Direct3D objects inside i_render. Any of them leads to the device: the device
// itself answers QueryInterface, everything else has GetDevice().
static IDirect3DDevice9 *g_dev;
static void *g_dev_anchor;         // the pointer inside i_render the device was found through
static size_t g_dev_off;

template <class T> static IDirect3DDevice9 *device_of(IUnknown *u)
{
    T *t = 0;
    IDirect3DDevice9 *dev = 0;
    if (FAILED(u->QueryInterface(__uuidof(T), (void **)&t)) || !t) return 0;
    if (FAILED(t->GetDevice(&dev))) dev = 0;
    t->Release();
    return dev;
}

static IDirect3DDevice9 *find_device(ObjMan *om)
{
    static int tries;
    Object *render = engine_find(om, "i_render", "render");
    if (!render) { if (!tries++) mp_log("ui: no i_render object"); return 0; }
    if (g_dev && *(void **)((char *)render + g_dev_off) == g_dev_anchor) return g_dev;
    g_dev = 0;
    HMODULE d3d = GetModuleHandleA("d3d9.dll");
    if (!d3d) return 0;
    char *lo = (char *)d3d;
    char *hi = lo + ((IMAGE_NT_HEADERS *)(lo + ((IMAGE_DOS_HEADER *)lo)->e_lfanew))->OptionalHeader.SizeOfImage;
    for (size_t off = sizeof(Object); off < 0x2000 && readable((char *)render + off, 8); off += 8) {
        void *p = *(void **)((char *)render + off);
        if (!readable(p, 8)) continue;
        char *vt = *(char **)p;
        if (vt < lo || vt >= hi) continue;
        IUnknown *u = (IUnknown *)p;
        IDirect3DDevice9 *dev = 0;
        if (FAILED(u->QueryInterface(__uuidof(IDirect3DDevice9), (void **)&dev))) dev = 0;
        if (!dev) dev = device_of<IDirect3DSwapChain9>(u);
        if (!dev) dev = device_of<IDirect3DSurface9>(u);
        if (!dev) dev = device_of<IDirect3DBaseTexture9>(u);
        if (!dev) dev = device_of<IDirect3DVertexBuffer9>(u);
        if (!dev) dev = device_of<IDirect3DIndexBuffer9>(u);
        if (!dev) dev = device_of<IDirect3DStateBlock9>(u);
        if (!dev) dev = device_of<IDirect3DVertexDeclaration9>(u);
        if (!dev) continue;
        dev->Release();            // the engine owns it and outlives every frame we draw in
        g_dev = dev;
        g_dev_anchor = p;
        g_dev_off = off;
        mp_log("ui: Direct3D device %p (through i_render+0x%zx)", dev, off);
        return dev;
    }
    if (tries++ % 600 == 0) mp_log("ui: no Direct3D device found in i_render");
    return 0;
}

// ---------------------------------------------------------------- text textures
struct TextTex { IDirect3DTexture9 *tex; int w, h; };
static std::unordered_map<std::string, TextTex> g_text;
static IDirect3DDevice9 *g_text_dev;

static void drop_text(void)
{
    for (auto &kv : g_text) if (kv.second.tex) kv.second.tex->Release();
    g_text.clear();
}

static TextTex *text_tex(IDirect3DDevice9 *dev, const char *s, int px, bool bold)
{
    if (dev != g_text_dev || g_text.size() > 96) { drop_text(); g_text_dev = dev; }
    char keybuf[320];
    sprintf_s(keybuf, "%d%c%.300s", px, bold ? 'b' : 'n', s);
    auto it = g_text.find(keybuf);
    if (it != g_text.end()) return &it->second;

    TextTex t = { 0, 0, 0 };
    HDC dc = CreateCompatibleDC(0);
    HFONT font = CreateFontA(-px, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, "Segoe Print");
    HGDIOBJ old_font = SelectObject(dc, font);
    SIZE sz = { 0, 0 };
    int len = (int)strlen(s);
    GetTextExtentPoint32A(dc, s, len, &sz);
    int w = sz.cx + 4, h = sz.cy + 2;
    if (w > 0 && h > 0 && w <= 4096) {
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        void *bits = 0;
        HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, 0, 0);
        if (bmp) {
            HGDIOBJ old_bmp = SelectObject(dc, bmp);
            memset(bits, 0, (size_t)w * h * 4);
            SetTextColor(dc, RGB(255, 255, 255));
            SetBkMode(dc, TRANSPARENT);
            TextOutA(dc, 2, 1, s, len);
            GdiFlush();
            if (SUCCEEDED(dev->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t.tex, 0))) {
                D3DLOCKED_RECT lr;
                if (SUCCEEDED(t.tex->LockRect(0, &lr, 0, 0))) {
                    for (int y = 0; y < h; y++) {
                        const unsigned char *src = (const unsigned char *)bits + (size_t)y * w * 4;
                        DWORD *dst = (DWORD *)((char *)lr.pBits + (size_t)y * lr.Pitch);
                        for (int x = 0; x < w; x++) {
                            unsigned a = src[x * 4];
                            if (src[x * 4 + 1] > a) a = src[x * 4 + 1];
                            if (src[x * 4 + 2] > a) a = src[x * 4 + 2];
                            dst[x] = (a << 24) | 0xffffff;
                        }
                    }
                    t.tex->UnlockRect(0);
                }
                t.w = w;
                t.h = h;
            }
            SelectObject(dc, old_bmp);
            DeleteObject(bmp);
        }
    }
    SelectObject(dc, old_font);
    DeleteObject(font);
    DeleteDC(dc);
    return &(g_text[keybuf] = t);
}

// ---------------------------------------------------------------- drawing
struct Vtx { float x, y, z, rhw; DWORD color; float u, v; };
static IDirect3DDevice9 *g_draw;
static float g_w, g_h, g_s;

static void quad(float x, float y, float w, float h, DWORD color)
{
    Vtx v[4] = {
        { x - 0.5f, y - 0.5f, 0, 1, color, 0, 0 }, { x + w - 0.5f, y - 0.5f, 0, 1, color, 1, 0 },
        { x - 0.5f, y + h - 0.5f, 0, 1, color, 0, 1 }, { x + w - 0.5f, y + h - 0.5f, 0, 1, color, 1, 1 },
    };
    g_draw->SetTexture(0, 0);
    g_draw->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    g_draw->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    g_draw->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(Vtx));
}

static void frame(float x, float y, float w, float h, float t, DWORD color)
{
    quad(x, y, w, t, color);
    quad(x, y + h - t, w, t, color);
    quad(x, y, t, h, color);
    quad(x + w - t, y, t, h, color);
}

// align: 0 left, 1 centre, 2 right (around x). Returns the text width in pixels.
static float text(const char *s, float x, float y, float size, DWORD color, int align = 1, bool bold = false)
{
    if (!s || !*s) return 0;
    TextTex *t = text_tex(g_draw, s, (int)(size * g_s + 0.5f), bold);
    if (!t || !t->tex) return 0;
    float w = (float)t->w, h = (float)t->h;
    if (align == 1) x -= w / 2;
    else if (align == 2) x -= w;
    x = floorf(x);
    y = floorf(y);
    Vtx v[4] = {
        { x - 0.5f, y - 0.5f, 0, 1, color, 0, 0 }, { x + w - 0.5f, y - 0.5f, 0, 1, color, 1, 0 },
        { x - 0.5f, y + h - 0.5f, 0, 1, color, 0, 1 }, { x + w - 0.5f, y + h - 0.5f, 0, 1, color, 1, 1 },
    };
    g_draw->SetTexture(0, t->tex);
    g_draw->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    g_draw->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    g_draw->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    g_draw->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(Vtx));
    return w;
}

static const DWORD k_ink = 0xff2a2622, k_paper = 0xf2ece6da, k_paper_hot = 0xfffff6d8, k_edge = 0xff5a5148, k_dim = 0xb0101010;

static bool button(const char *label, float cx, float y, float w, float h, float size = 26)
{
    float x = cx - w / 2;
    bool hot = g_mx >= x && g_mx < x + w && g_my >= y && g_my < y + h;
    quad(x, y, w, h, hot ? k_paper_hot : k_paper);
    frame(x, y, w, h, hot ? 3 * g_s : 2 * g_s, k_edge);
    text(label, cx, y + h / 2 - size * 0.81f * g_s, size, k_ink, 1, true);
    return hot && g_clicked;
}

// ---------------------------------------------------------------- menu logic
static void save_address(void)
{
    char path[MAX_PATH];
    sprintf_s(path, "%s\\users\\mp_last_host.txt", g_root);
    FILE *f = 0;
    if (!fopen_s(&f, path, "w") && f) { fprintf(f, "%s\n", g_address); fclose(f); }
}

static void load_address(void)
{
    char path[MAX_PATH];
    sprintf_s(path, "%s\\users\\mp_last_host.txt", g_root);
    FILE *f = 0;
    if (!fopen_s(&f, path, "r") && f) {
        if (fgets(g_address, sizeof g_address, f)) g_address[strcspn(g_address, "\r\n")] = 0;
        fclose(f);
    }
}

static void type_address(void)
{
    size_t n = strlen(g_address);
    bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0, shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    auto add = [&](char c) { if (n + 1 < sizeof g_address) { g_address[n++] = c; g_address[n] = 0; } };
    for (int k = '0'; k <= '9'; k++) if (key_pressed(k) && !shift) add((char)k);
    for (int k = VK_NUMPAD0; k <= VK_NUMPAD9; k++) if (key_pressed(k)) add((char)('0' + k - VK_NUMPAD0));
    bool paste = false;
    for (int k = 'A'; k <= 'Z'; k++)
        if (key_pressed(k)) {
            if (ctrl && k == 'V') paste = true;
            else if (!ctrl) add((char)(k + 32));
        }
    if (key_pressed(VK_OEM_PERIOD) || key_pressed(VK_DECIMAL)) add('.');
    if (key_pressed(VK_OEM_MINUS)) add('-');
    if (key_pressed(VK_OEM_1) && shift) add(':');
    if (key_pressed(VK_BACK) && n) g_address[--n] = 0;
    if (key_pressed(VK_DELETE)) g_address[n = 0] = 0;
    if (paste && OpenClipboard(0)) {
        if (HANDLE h = GetClipboardData(CF_TEXT)) {
            if (const char *p = (const char *)GlobalLock(h)) {
                n = 0;
                for (; *p && n + 1 < sizeof g_address; p++)
                    if (isalnum((unsigned char)*p) || *p == '.' || *p == ':' || *p == '-') g_address[n++] = *p;
                g_address[n] = 0;
                GlobalUnlock(h);
            }
        }
        CloseClipboard();
    }
}

// ---------------------------------------------------------------- multiplayer operation saves
// The host's battle program copies the operation saves to users\mp_saves\<host>_vs_<player> every time an
// operation battle starts (see session.cpp). The operation lobby lists those folders; RESTORE copies one
// back into the profile, so the game's Operations screen continues from it. What it replaces is kept in
// users\mp_saves\_replaced.
struct SaveSlot { char name[64], detail[96]; };
static std::vector<SaveSlot> g_slots;
static DWORD g_slots_read;

static bool profile_dir(char *out, size_t size)
{
    char path[MAX_PATH], text[512] = "", id[64] = "cloud";
    sprintf_s(path, "%s\\users\\profiles\\cur_profile.engcfg", g_root);
    FILE *f = 0;
    if (!fopen_s(&f, path, "rb") && f) {
        text[fread(text, 1, sizeof text - 1, f)] = 0;
        fclose(f);
        const char *p = strstr(text, "cur_id");
        if (p && (p = strchr(p, '='))) {
            for (p++; *p == ' ' || *p == '\t'; p++) {}
            size_t k = strcspn(p, ";\r\n");
            if (k && k < sizeof id) { memcpy(id, p, k); id[k] = 0; }
        }
    }
    sprintf_s(out, size, "%s\\users\\profiles\\%s", g_root, id);
    return true;
}

static void read_slots(void)
{
    g_slots.clear();
    g_slots_read = GetTickCount();
    char find[MAX_PATH];
    WIN32_FIND_DATAA fd, sd;
    sprintf_s(find, "%s\\users\\mp_saves\\*", g_root);
    HANDLE h = FindFirstFileA(find, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.' || fd.cFileName[0] == '_') continue;
        SaveSlot slot = {};
        strncpy_s(slot.name, fd.cFileName, sizeof slot.name - 1);
        // newest operation save in the folder: its name and time describe the slot
        FILETIME best = {};
        sprintf_s(find, "%s\\users\\mp_saves\\%s\\*.opsave", g_root, fd.cFileName);
        HANDLE s = FindFirstFileA(find, &sd);
        if (s != INVALID_HANDLE_VALUE) {
            do {
                if (CompareFileTime(&sd.ftLastWriteTime, &best) <= 0) continue;
                best = sd.ftLastWriteTime;
                FILETIME local;
                SYSTEMTIME st;
                FileTimeToLocalFileTime(&best, &local);
                FileTimeToSystemTime(&local, &st);
                char op[48];
                strncpy_s(op, sd.cFileName, sizeof op - 1);
                if (char *dot = strrchr(op, '.')) *dot = 0;
                sprintf_s(slot.detail, "%s, saved %02u.%02u. %02u:%02u", op, st.wDay, st.wMonth, st.wHour, st.wMinute);
            } while (FindNextFileA(s, &sd));
            FindClose(s);
        }
        if (slot.detail[0]) g_slots.push_back(slot);
    } while (FindNextFileA(h, &fd) && g_slots.size() < 3);
    FindClose(h);
}

static void restore_slot(const SaveSlot &slot)
{
    char from[MAX_PATH], to[MAX_PATH], keep[MAX_PATH], find[MAX_PATH], a[MAX_PATH], b[MAX_PATH];
    WIN32_FIND_DATAA fd;
    profile_dir(to, sizeof to);
    sprintf_s(from, "%s\\users\\mp_saves\\%s", g_root, slot.name);
    sprintf_s(keep, "%s\\users\\mp_saves\\_replaced", g_root);
    CreateDirectoryA(keep, 0);
    sprintf_s(find, "%s\\*", from);
    int n = 0;
    HANDLE h = FindFirstFileA(find, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            sprintf_s(a, "%s\\%s", to, fd.cFileName);
            sprintf_s(b, "%s\\%s", keep, fd.cFileName);
            CopyFileA(a, b, FALSE);                     // what is about to be replaced
            sprintf_s(b, "%s\\%s", from, fd.cFileName);
            if (CopyFileA(b, a, FALSE)) n++;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    sprintf_s(g_message, "Restored %s (%d files). Continue it from the Operations screen.", slot.name, n);
    mp_log("ui: restored multiplayer operation save %s (%d files)", slot.name, n);
}

void ui_start(ObjMan *)
{
    g_tactical = !strcmp(g_program, "tacgame");
    strcpy_s(g_root, g_out_dir);
    if (char *s = strrchr(g_root, '\\')) *s = 0;
    load_address();
    // Back from a joined battle: this game is an ordinary single-player game again until told otherwise.
    if (!g_tactical && session_role() == ROLE_JOIN && !session_map_mode()) session_single_player();
    if (!g_tactical && GetEnvironmentVariableA("MP_AUTOJOIN", g_autojoin, sizeof g_autojoin)) SetEnvironmentVariableA("MP_AUTOJOIN", 0);
    else g_autojoin[0] = 0;
}

void ui_frame(ObjMan *om)
{
    g_frames++;
    if (g_exit_at && GetTickCount() > g_exit_at) ExitProcess(0);
    HWND win = main_window();
    bool focused = win && GetForegroundWindow() == win;

    // mouse in back-buffer pixels
    POINT p;
    RECT rc;
    if (win && GetCursorPos(&p) && ScreenToClient(win, &p) && GetClientRect(win, &rc) && rc.right > 0 && rc.bottom > 0 && g_w > 0) {
        g_mx = p.x * g_w / rc.right;
        g_my = p.y * g_h / rc.bottom;
    }
    g_mouse_was_down = g_mouse_down;
    g_mouse_down = focused && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    g_clicked = g_mouse_was_down && !g_mouse_down;      // act on release

    if (g_tactical) return;
    g_on_main_menu = on_main_menu(om);
    g_on_op_map = session_on_op_map(om);
    if (!g_on_main_menu || g_screen != SCR_NONE) g_button_hot = false;
    if (!g_on_op_map || g_screen != SCR_NONE) g_map_button_hot = false;
    // back from a turn on the operational map: into the host's lobby again, ready for the battles
    if (g_autojoin[0] && g_on_main_menu && g_frames > 45 && g_screen == SCR_NONE) {
        strcpy_s(g_address, g_autojoin);
        g_autojoin[0] = 0;
        g_message[0] = 0;
        mp_log("ui: rejoining the lobby of '%s'", g_address);
        g_joiner.prefer_side = 0;
        g_joiner.lobby_ready = true;
        g_joiner.swap_req = 0;
        strcpy_s(g_joiner.my_name, session_my_name());
        if (g_joiner.start(g_address)) { g_lobby_host = false; g_screen = SCR_LOBBY; }
    }
    // While a turn is being ended nothing is drawn over the map - the players want to see the moves being
    // made; the line at the top says what is going on. The map only takes no clicks for the moment in
    // which the moves travel between the games.
    g_map_busy = g_on_op_map && session_map_blocking();
    bool hosting = session_role() == ROLE_HOST;
    Action act = g_action;
    g_action = ACT_NONE;
    if (act == ACT_OPEN) {
        g_message[0] = 0;
        g_lobby_host = hosting;
        g_screen = hosting ? SCR_LOBBY : SCR_MAIN;
        g_button_hot = false;
        mp_log("ui: multiplayer menu opened");
    } else if (act == ACT_HOST_SKIRMISH || act == ACT_HOST_MISSION || act == ACT_HOST_OPERATION) {
        int mode = act == ACT_HOST_OPERATION ? HOST_MODE_OPERATION : act == ACT_HOST_MISSION ? HOST_MODE_MISSION : HOST_MODE_SKIRMISH;
        session_host(mode);
        mp_log("ui: hosting %s chosen", mode == HOST_MODE_OPERATION ? "operation" : mode == HOST_MODE_MISSION ? "mission" : "skirmish");
        g_message[0] = 0;
        g_lobby_host = true;
        g_screen = SCR_LOBBY;
    } else if (act == ACT_SETUP && hosting) {
        close_menu();
        open_stock_screen(om, session_host_mode());
    } else if (act == ACT_SWAP) {
        if (g_lobby_host) session_toggle_swap();
        else g_joiner.swap_req++;
    } else if (act == ACT_READY) {
        g_joiner.lobby_ready = !g_joiner.lobby_ready;
        mp_log("ui: lobby: %s", g_joiner.lobby_ready ? "ready" : "not ready");
    } else if (act == ACT_LEAVE) {
        if (g_lobby_host) {
            session_single_player();
            mp_log("ui: stopped hosting");
            g_screen = SCR_MAIN;
        } else {
            g_joiner.cancel();
            g_screen = SCR_JOIN;
        }
    } else if (act == ACT_CONNECT && g_address[0]) {
        save_address();
        g_message[0] = 0;
        mp_log("ui: connecting to '%s'", g_address);
        g_joiner.prefer_side = 0;
        g_joiner.lobby_ready = false;
        g_joiner.swap_req = 0;
        strcpy_s(g_joiner.my_name, session_my_name());
        if (g_joiner.start(g_address)) { g_lobby_host = false; g_screen = SCR_LOBBY; }
        else strcpy_s(g_message, g_joiner.error);
    } else if (act == ACT_BACK) {
        if (g_screen == SCR_JOIN) g_screen = SCR_MAIN;
        else close_menu();
    } else if (act == ACT_RESTORE && g_action_arg >= 0 && g_action_arg < (int)g_slots.size()) {
        restore_slot(g_slots[g_action_arg]);
    }

    // F9: the lobby while hosting (the host is usually on the game's Skirmish or Operations screen then)
    if (focused && key_pressed(VK_F9) && g_screen == SCR_NONE && (hosting || g_on_main_menu)) g_action = ACT_OPEN;
    if (g_screen == SCR_NONE) return;

    bool escape = focused && key_pressed(VK_ESCAPE);
    if (g_screen == SCR_JOIN) {
        if (focused) type_address();
        if (focused && key_pressed(VK_RETURN)) g_action = ACT_CONNECT;
        if (escape) g_action = ACT_BACK;
    } else if (g_screen == SCR_MAIN) {
        if (escape) g_action = ACT_BACK;
    } else if (g_screen == SCR_LOBBY && !g_lobby_host) {
        g_joiner.poll();
        if (g_joiner.state == Joiner::FAILED) {
            strcpy_s(g_message, g_joiner.error);
            g_screen = SCR_JOIN;
        } else if (g_joiner.state == Joiner::DONE) {
            bool map = g_joiner.want_map;
            if (map ? g_joiner.launch_map(g_root) : g_joiner.launch(g_root)) {
                if (map) mp_log("ui: operational map received from %s, opening it", g_address);
                else mp_log("ui: battle received from %s, starting it", g_address);
                g_screen = SCR_STARTING;
                g_exit_at = GetTickCount() + 2500;       // the battle is a separate program; this one is done
            } else {
                strcpy_s(g_message, g_joiner.error);
                g_screen = SCR_JOIN;
            }
        }
    } else if (g_screen == SCR_LOBBY && g_lobby_host) {
        if (session_host_mode() == HOST_MODE_OPERATION && GetTickCount() - g_slots_read > 2000) read_slots();
        if (session_role() != ROLE_HOST) g_screen = SCR_MAIN;
    }
}

// The MULTIPLAYER entry of the main menu: same hand-written capitals as the stock entries, underlined
// while the mouse is on it, at the top of the list above PLAYER PROFILE (the only free place: the stock
// entries cannot be moved from here).
static const DWORD k_menu_ink = 0xff26221e;

static void draw_main_menu_button(void)
{
    const float size = 30;
    float cx = g_w / 2, cy = g_h * 0.129f;
    TextTex *t = text_tex(g_draw, "MULTIPLAYER", (int)(size * g_s + 0.5f), false);
    float w = t ? (float)t->w : 0, h = t ? (float)t->h : 0;
    bool hot = fabsf(g_mx - cx) < w / 2 + 12 * g_s && fabsf(g_my - cy) < h * 0.36f;
    g_button_hot = hot;
    text("MULTIPLAYER", cx, cy - h / 2, size, k_menu_ink);
    if (hot) quad(cx - w / 2 + 2 * g_s, cy + h * 0.30f, w - 4 * g_s, 2.5f * g_s, k_menu_ink);
    if (hot && g_clicked) g_action = ACT_OPEN;
}

// Shown while the turn is being ended: on the host while the other player's moves are fetched and merged,
// on the joining game while its moves are sent.
static void draw_map_wait(float cx, float py, float bw, float bh)
{
    float s = g_s;
    char peer[24] = "the other player", line[160];
    session_peer(peer, sizeof peer, 0);
    (void)bw; (void)bh;
    text("OPERATIONAL MAP", cx, py + 34 * s, 40, k_ink, 1, true);
    if (session_map_mode()) {
        text("Both players have ended the turn.", cx, py + 180 * s, 26, k_ink);
        text("Your moves go to the host's game, which carries the turn out. The map reloads with the result -", cx, py + 236 * s, 17, k_edge);
        text("or, if the host starts a battle of this turn, your game enters that battle.", cx, py + 262 * s, 17, k_edge);
    } else {
        sprintf_s(line, "Ending the turn with %s's moves...", peer);
        text(line, cx, py + 180 * s, 26, k_ink);
        text("His moves are fetched and put into your operation; the map reloads and the turn ends.", cx, py + 236 * s, 17, k_edge);
    }
}

static void draw_lobby(float cx, float px, float py, float pw, float ph)
{
    float s = g_s, bw = 560 * s, bh = 76 * s, hw = 272 * s;
    float left = cx - bw / 2 + hw / 2, right = cx + bw / 2 - hw / 2;
    float r1 = py + 430 * s, r2 = py + 530 * s, r3 = py + 630 * s;
    const DWORD good = 0xff2e6b2a, wait = 0xff8a1c10;
    char line[220], peer[24];
    int mode;
    bool operation, swap, peer_here, peer_ready = false, starting = g_screen == SCR_STARTING;
    const char *host_name, *join_name;

    if (g_lobby_host) {
        mode = session_host_mode();
        swap = session_swap();
        peer_here = session_peer(peer, sizeof peer, &peer_ready);
        host_name = session_my_name();
        join_name = peer_here ? peer : 0;
    } else {
        mode = g_joiner.host_mode;
        swap = g_joiner.swap;
        peer_here = g_joiner.state != Joiner::CONNECTING && g_joiner.host_name[0];
        peer_ready = g_joiner.lobby_ready;
        host_name = peer_here ? g_joiner.host_name : 0;
        join_name = g_joiner.my_name;
    }

    operation = mode == HOST_MODE_OPERATION;
    bool mission = mode == HOST_MODE_MISSION;
    text("MULTIPLAYER LOBBY", cx, py + 30 * s, 40, k_ink, 1, true);
    if (g_lobby_host || peer_here) text(operation ? "Operation" : mission ? "Custom mission" : "Skirmish", cx, py + 92 * s, 22, k_edge);

    // the two players: name, the force each commands, state
    float nx = px + 60 * s, fx = px + 300 * s, sx = px + pw - 60 * s, y1 = py + 150 * s, y2 = py + 204 * s;
    const char *host_force = operation ? "the side of the operation" : mission ? (swap ? "the mission's ENEMY force" : "the mission's PLAYER force")
                           : swap ? "ENEMY force of the battle setup" : "OWN force of the battle setup";
    const char *join_force = operation ? "the opposing side" : mission ? (swap ? "the mission's PLAYER force" : "the mission's ENEMY force")
                           : swap ? "OWN force of the battle setup" : "ENEMY force of the battle setup";
    if (host_name) {
        sprintf_s(line, "%s  (host)", host_name);
        text(line, nx, y1, 22, k_ink, 0, true);
        text(host_force, fx, y1 + 3 * s, 17, k_ink, 0);
        if (!g_lobby_host) text(g_joiner.host_in_battle ? "in the battle" : "setting up", sx, y1 + 3 * s, 17, g_joiner.host_in_battle ? good : k_edge, 2);
    } else {
        sprintf_s(line, "Looking for the host at %s ...  (%u s)", g_address, g_joiner.waited_seconds());
        text(line, nx, y1, 20, wait, 0);
    }
    if (join_name) {
        text(join_name, nx, y2, 22, k_ink, 0, true);
        text(join_force, fx, y2 + 3 * s, 17, k_ink, 0);
        text(peer_ready ? "READY" : "not ready", sx, y2 + 3 * s, 17, peer_ready ? good : wait, 2);
    } else {
        sprintf_s(line, "Waiting for a player to join at  %s : %u", session_local_ip()[0] == '?' ? "your address" : session_local_ip(), session_port());
        text(line, nx, y2, 20, wait, 0);
    }
    quad(px + 50 * s, py + 252 * s, pw - 100 * s, 2 * s, k_edge);

    float y = py + 266 * s;
    if (operation) {
        text("The host opens the operation from the side he wants to play; modifiers are set on the Operations screen.", cx, y, 15, k_edge);
        text("On the operational map both players move their own forces at the same time; the turn ends when both have pressed next turn.", cx, y + 24 * s, 15, k_edge);
    } else if (mission) {
        text("The host picks a mission on the Missions screen: the prepared ones and those made in the battle editor.", cx, y, 15, k_edge);
        text("The mission travels to the other player with the battle. SWAP SIDES exchanges who commands which force.", cx, y + 24 * s, 15, k_edge);
    } else {
        text("The host sets up both forces on the Skirmish screen. SWAP SIDES exchanges who commands which.", cx, y, 15, k_edge);
        if (g_lobby_host) text("Over the internet the other player uses your public address (UDP port 1943 forwarded to this PC).", cx, y + 24 * s, 15, k_edge);
    }

    if (starting) {
        text(g_joiner.want_map ? "Operational map received. Opening it..." : "Battle received. Starting...", cx, r1 + 16 * s, 24, good);
        return;
    }
    if (g_lobby_host) {
        if (operation) {
            // multiplayer saves of earlier operation battles
            float sy = py + 322 * s;
            if (g_slots.empty()) text("No multiplayer operation saves yet. One is written every time an operation battle starts.", cx, sy + 8 * s, 15, k_edge);
            for (size_t i = 0; i < g_slots.size(); i++, sy += 34 * s) {
                sprintf_s(line, "%s  -  %s", g_slots[i].name, g_slots[i].detail);
                text(line, nx, sy, 16, k_ink, 0);
                if (button("RESTORE", px + pw - 110 * s, sy - 2 * s, 120 * s, 30 * s, 14)) { g_action = ACT_RESTORE; g_action_arg = (int)i; }
            }
        } else if (button("SWAP SIDES", cx, r1, bw, bh)) {
            g_action = ACT_SWAP;
        }
        if (button(operation ? "CHOOSE THE OPERATION" : mission ? "CHOOSE THE MISSION" : "SET UP THE BATTLE", cx, r2, bw, bh)) g_action = ACT_SETUP;
        if (button("LEAVE THE LOBBY", cx, r3, bw, bh)) g_action = ACT_LEAVE;
        text(g_message, cx, py + ph - 30 * s, 15, good);
    } else {
        if (g_joiner.state == Joiner::RECEIVING) {
            sprintf_s(line, g_joiner.want_map ? "Receiving the operational map...  %d%%" : "Receiving the battle...  %d%%", (int)(g_joiner.progress() * 100));
            text(line, cx, r1 + 16 * s, 24, good);
        } else {
            if (operation || !peer_here) {
                if (button(g_joiner.lobby_ready ? "NOT READY" : "READY", cx, r1, bw, bh)) g_action = ACT_READY;
            } else {
                if (button(g_joiner.lobby_ready ? "NOT READY" : "READY", left, r1, hw, bh)) g_action = ACT_READY;
                if (button("SWAP SIDES", right, r1, hw, bh)) g_action = ACT_SWAP;
            }
            const char *hint = !peer_here ? (g_joiner.waited_seconds() >= 8 ? "No answer yet. Check the address and that UDP port 1943 reaches the host." : "")
                             : !g_joiner.lobby_ready ? (g_joiner.host_in_battle ? "The host is in the battle. Press READY to enter it." : "Press READY when you are; the battle starts when the host starts it.")
                             : "Ready. Waiting for the host to start the battle...";
            text(hint, cx, r2 + 22 * s, 17, g_joiner.lobby_ready ? good : wait);
        }
        if (button("LEAVE THE LOBBY", cx, r3, bw, bh)) g_action = ACT_LEAVE;
    }
}

static void draw_menu(void)
{
    float cx = g_w / 2, s = g_s;
    float pw = 760 * s, ph = 740 * s, px = cx - pw / 2, py = g_h / 2 - ph / 2;
    if (g_screen == SCR_LOBBY || g_screen == SCR_STARTING) { pw = 980 * s; px = cx - pw / 2; }
    quad(0, 0, g_w, g_h, k_dim);
    quad(px, py, pw, ph, 0xf6e9e2d4);
    frame(px, py, pw, ph, 3 * s, k_edge);
    float bw = 560 * s, bh = 76 * s, y = py + 150 * s, gap = 100 * s;

    if (g_screen == SCR_MAIN) {
        text("MULTIPLAYER", cx, py + 34 * s, 46, k_ink, 1, true);
        text("Two players: one hosts, the other joins", cx, py + 100 * s, 20, k_edge);
        if (button("HOST A SKIRMISH", cx, y, bw, bh)) g_action = ACT_HOST_SKIRMISH;
        if (button("HOST A CUSTOM MISSION", cx, y + gap, bw, bh)) g_action = ACT_HOST_MISSION;
        if (button("HOST AN OPERATION", cx, y + 2 * gap, bw, bh)) g_action = ACT_HOST_OPERATION;
        if (button("JOIN A GAME", cx, y + 3 * gap, bw, bh)) { g_message[0] = 0; g_screen = SCR_JOIN; }
        if (button("BACK", cx, y + 4 * gap, bw, bh)) g_action = ACT_BACK;
    } else if (g_screen == SCR_JOIN) {
        text("JOIN A GAME", cx, py + 34 * s, 40, k_ink, 1, true);
        text("Host address (type it, or Ctrl+V to paste)", cx, py + 104 * s, 20, k_edge);
        float fw = 560 * s, fh = 70 * s, fx = cx - fw / 2, fy = y + 6 * s;
        quad(fx, fy, fw, fh, 0xfffffbf0);
        frame(fx, fy, fw, fh, 2 * s, k_edge);
        char shown[80];
        sprintf_s(shown, "%s%s", g_address, (GetTickCount() / 500) % 2 ? "_" : " ");
        text(shown, fx + 18 * s, fy + fh / 2 - 21 * s, 26, k_ink, 0);
        if (button("CONNECT", cx, y + gap, bw, bh)) g_action = ACT_CONNECT;
        if (button("BACK", cx, y + 2 * gap, bw, bh)) g_action = ACT_BACK;
        text(g_message, cx, py + ph - 60 * s, 17, 0xff8a1c10);
    } else if (g_screen == SCR_MAPWAIT) {
        draw_map_wait(cx, py, bw, bh);
    } else {
        draw_lobby(cx, px, py, pw, ph);
    }
}

// ---------------------------------------------------------------- test support: screenshots of the game's own frame
// With MP_TEST=1 the module watches for out\mp_cmd_<pid>.txt containing "shot <name>" or "ready" and writes the frame
// it has just drawn to out\<name>.bmp (half size). Tests use this instead of grabbing the desktop, so they
// work whatever else is on screen and never capture other windows.
static bool test_shot_request(char *name, size_t n)
{
    static int enabled = -1;
    if (enabled < 0) enabled = GetEnvironmentVariableA("MP_TEST", 0, 0) != 0;
    if (!enabled || g_frames % 6) return false;
    char path[MAX_PATH], line[128] = "";
    sprintf_s(path, "%s\\mp_cmd_%lu.txt", g_out_dir, GetCurrentProcessId());
    FILE *f = 0;
    if (fopen_s(&f, path, "r") || !f) return false;
    fgets(line, sizeof line, f);
    fclose(f);
    DeleteFileA(path);
    if (!strncmp(line, "ready", 5)) { session_test_ready(); return false; }
    if (!strncmp(line, "resign", 6)) { session_test_resign(); return false; }
    if (!strncmp(line, "endturn", 7)) { session_test_next_turn(); return false; }
    if (!strncmp(line, "savemap", 7)) { session_test_save(); return false; }
    if (!strncmp(line, "newmap", 6)) { session_test_new_map(); return false; }
    if (!strncmp(line, "dig ", 4)) { int n = 0; float m = 0; if (sscanf_s(line + 4, "%d %f", &n, &m) == 2) trench_test_move(n, m); return false; }
    return sscanf_s(line, "shot %63s", name, (unsigned)n) == 1;
}

static void save_frame(IDirect3DDevice9 *dev, const char *name)
{
    IDirect3DSurface9 *rt = 0, *resolved = 0, *mem = 0;
    D3DSURFACE_DESC d;
    D3DLOCKED_RECT lr;
    if (FAILED(dev->GetRenderTarget(0, &rt)) || !rt) return;
    rt->GetDesc(&d);
    bool ok = SUCCEEDED(dev->CreateRenderTarget(d.Width, d.Height, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &resolved, 0)) &&
              SUCCEEDED(dev->StretchRect(rt, 0, resolved, 0, D3DTEXF_NONE)) &&
              SUCCEEDED(dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &mem, 0)) &&
              SUCCEEDED(dev->GetRenderTargetData(resolved, mem)) && SUCCEEDED(mem->LockRect(&lr, 0, D3DLOCK_READONLY));
    if (ok) {
        int w = d.Width / 2, h = d.Height / 2, stride = (w * 3 + 3) & ~3;
        char path[MAX_PATH];
        sprintf_s(path, "%s\\%s.bmp", g_out_dir, name);
        FILE *f = 0;
        if (!fopen_s(&f, path, "wb") && f) {
            BITMAPFILEHEADER fh = { 0x4d42, (DWORD)(54 + stride * h), 0, 0, 54 };
            BITMAPINFOHEADER ih = { 40, w, -h, 1, 24, 0, (DWORD)(stride * h) };
            fwrite(&fh, sizeof fh, 1, f);
            fwrite(&ih, sizeof ih, 1, f);
            std::string row(stride, 0);
            for (int y = 0; y < h; y++) {
                const unsigned char *src = (const unsigned char *)lr.pBits + (size_t)(y * 2) * lr.Pitch;
                for (int x = 0; x < w; x++) memcpy(&row[x * 3], src + x * 8, 3);     // X8R8G8B8 / A8R8G8B8: b, g, r
                fwrite(row.data(), 1, stride, f);
            }
            fclose(f);
        }
        mem->UnlockRect();
    }
    mp_log("test: frame %s %s (%ux%u, format %d)", name, ok ? "saved" : "could not be captured", d.Width, d.Height, d.Format);
    if (mem) mem->Release();
    if (resolved) resolved->Release();
    rt->Release();
}

void ui_render(ObjMan *om)
{
    const char *status = session_status();
    bool hint = !g_tactical && g_screen == SCR_NONE && g_on_main_menu;        // the main menu's MULTIPLAYER entry
    char shot[64] = "";
    test_shot_request(shot, sizeof shot);
    if (g_screen == SCR_NONE && !status[0] && !hint && !shot[0] && !g_on_op_map) {
        if (!g_text.empty() && g_frames % 600 == 0) drop_text();       // nothing shown: hold no device resources
        return;
    }
    IDirect3DDevice9 *dev = find_device(om);
    if (!dev) return;
    D3DVIEWPORT9 vp;
    if (FAILED(dev->GetViewport(&vp)) || !vp.Width || !vp.Height) return;
    g_w = (float)vp.Width;
    g_h = (float)vp.Height;
    g_s = g_h / 1080.0f;
    g_draw = dev;

    IDirect3DStateBlock9 *saved = 0;
    if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &saved)) || !saved) return;
    bool began = SUCCEEDED(dev->BeginScene());         // fails harmlessly when the engine already has a scene open
    dev->SetVertexShader(0);
    dev->SetPixelShader(0);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xf);
    dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);

    if (g_screen != SCR_NONE) {
        draw_menu();
    } else {
        if (hint) draw_main_menu_button();

        if (status[0]) {
            TextTex *t = text_tex(dev, status, (int)(17 * g_s + 0.5f), false);
            float w = (t ? t->w : 0) + 36 * g_s, h = 34 * g_s;
            quad(g_w / 2 - w / 2, 46 * g_s, w, h, 0xc0181410);
            text(status, g_w / 2, 46 * g_s + 3 * g_s, 17, session_connected() ? 0xffa8f0a0 : 0xfff2e6c8);
        }
    }

    if (shot[0]) save_frame(dev, shot);
    if (began) dev->EndScene();
    saved->Apply();
    saved->Release();
    g_clicked = false;
}
