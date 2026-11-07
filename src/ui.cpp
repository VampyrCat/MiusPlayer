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
// device-reset handling. A game that renders with Direct3D 11 (the 2026 beta's default) gets the same
// drawing through ui_d3d11.cpp, on the engine's own Direct3D 11 device.
#include "common.h"
#include "build.h"
#include "joiner.h"
#include "detour.h"
#include "ui_d3d11.h"
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
static bool g_autojoin_ready;    // ... ready (from the map) or not yet (Steam's "Join Game")
// CHOOSE MY FORCE (skirmish, joining player): the stock Skirmish screen is open for this player's own force;
// its start button does not start a battle but hands the force to the host (catch_battle_start)
static bool g_force_picking;
static bool g_host_on_skirmish;  // hosting a skirmish, its Skirmish screen up (the banner over it)
static char g_force_target[128], g_force_note[200];
static unsigned g_my_force_squads, g_my_force_side;
static DWORD g_block_until, g_exit_at, g_frames;
static Joiner g_joiner;
static char g_address[64], g_root[MAX_PATH], g_message[200];
// The two port fields, as typed: the port a game hosted here listens on, and the port of the host to join.
// Left empty they mean the default port, 1943, which the field then shows in grey.
static char g_host_port[8], g_join_port[8];
static bool g_upnp;              // the multiplayer panel's UPnP box: ask the router to open the hosting port
static SteamHost g_shosts[4];    // JOIN A GAME: the Steam friends who host a game right now
static int g_nshosts;
static DWORD g_shosts_at;
static char g_steam_host_name[64];
static int g_focus;              // JOIN A GAME: which field has the keyboard, 0 the address, 1 the port
enum Action { ACT_NONE, ACT_OPEN, ACT_HOST_SKIRMISH, ACT_HOST_MISSION, ACT_HOST_OPERATION, ACT_CONNECT, ACT_LEAVE, ACT_SETUP, ACT_SWAP,
              ACT_READY, ACT_BACK, ACT_RESTORE, ACT_MAP_GIVE, ACT_MAP_CANCEL, ACT_CEASEFIRE, ACT_CONNECT_STEAM, ACT_CHOOSE_FORCE,
              ACT_TEST_SKIRMISH_START };
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

static bool g_cease_hot;         // the mouse is on the ESC menu's CEASEFIRE button
bool ui_blocks_input(void) { return g_screen != SCR_NONE || g_button_hot || g_map_button_hot || g_map_busy || g_cease_hot || GetTickCount() < g_block_until; }
// The operational map to look at (the host fights the turn's battles): the game gets no mouse clicks - no
// orders, no battle started on this copy - but the view still moves (mouse, wheel, keys).
bool ui_blocks_clicks(void) { return g_on_op_map && session_map_view(); }

static void close_menu(void)
{
    g_screen = SCR_NONE;
    g_block_until = GetTickCount() + 250;      // the click that closed the menu must not reach the game
    SetEnvironmentVariableA("MP_MENU", "done");
}

// ---------------------------------------------------------------- engine hooks: main menu state, screen switch
// i_opgame +0x100 -> menu controller (a pointer to the i_opgame object itself, set by its constructor:
// adv_opgame+0x2bb50, beta26 +0x2bb40); controller +0x1258 = current screen (0 main menu, 2 campaign, 0x11
// skirmish; beta26 +0x1268, same numbers). Screen changes go through adv_opgame+0x762e0(controller, screen, 0)
// (beta26 +0x847c0); the stock menu buttons are thin stubs around it. The stubs are checked byte for byte
// before use, so a different game build simply skips them.
// The controller's fields this file reads, per build (beta26: read in its constructor and in the Skirmish
// screen's code, which use them as the 2024 builds do):
static size_t mc_screen(void) { return BF(0x1258, 0x1268); }   // current screen (the skirmish entry's cmp [rcx+...], 11h)
static size_t mc_mode(void)   { return BF(0x1200, 0x1210); }   // 4 in the menus, 0 on an operation's map (constructor: qword 4)
static size_t mc_date(void)   { return BF(0x1300, 0x1380); }   // Skirmish screen: year, season +4 (beta26 +0x6dc20 sets both)
static size_t mc_size(void)   { return BF(0x131c, 0x139c); }   // battle size 0..3 (the screen's slot rows, beta26 +0x6ef70)
static size_t mc_picks(void)  { return BF(0x1a74, 0x1af4); }   // the player's 0x84-byte picks, supports +0xf78 (beta26 +0x6ef70, +0x5d6f0)
static size_t mc_spec(void)   { return BF(0x1950, 0x19d0); }   // -> the setup: +0x308 own side, +0x30c enemy (beta26 +0x5e120)

static void *menu_controller(ObjMan *om)
{
    Object *og = engine_find(om, "i_opgame", "opgame");
    if (!og) return 0;
    void *c = *(void **)((char *)og + 0x100);
    return readable(c, mc_screen() + 8) ? c : 0;
}

static int menu_screen(ObjMan *om)
{
    void *c = menu_controller(om);
    return c ? *(int *)((char *)c + mc_screen()) : -1;
}

// The main menu itself: screen 0 while the program is in its menu mode. The operational map of an
// operation is screen 0 as well; controller +0x1200 tells them apart (4 in the menus, 0 on the map).
static bool on_main_menu(ObjMan *om)
{
    void *c = menu_controller(om);
    return c && *(int *)((char *)c + mc_screen()) == 0 && *(int *)((char *)c + mc_mode()) != 0;
}

// The stock Skirmish screen's start: the same handler as the entry that opens the screen (adv_opgame+0x627f0,
// beta26 +0x6daf0, "do_mm_skirmish" and "do_close_combat") - called while the screen is up it makes the
// battle and starts it. Checked once, before anything is hooked there (CHOOSE MY FORCE detours it).
// beta26's handler takes (controller, u64, 16-byte event *): it copies the 16 bytes into a lambda that holds
// the start back while the menu music fades out (adv_opgame+0x8cba0) and then calls the handler again from
// the fade tick (+0x8cd00), with controller +0x3dbc set to 1 for that call. Called from here it gets 16 zero
// bytes (the 2024 handler only keeps that argument on its stack: it is passed 0 there, as always).
static const unsigned char k_no_event[16] = {};
static u64 skirmish_event(void) { return g_bi == BUILD_BETA26 ? (u64)k_no_event : 0; }

static size_t skirmish_rva(void) { return BV(0x627f0, 0x624f0, 0x6daf0); }

static bool skirmish_stub_ok(void)
{
    static int ok = -1;
    if (ok < 0) {
        static const unsigned char head[] = { 0x4c, 0x89, 0x44, 0x24, 0x18, 0x48, 0x89, 0x54, 0x24, 0x10, 0x53, 0x48, 0x83, 0xec, 0x40,
                                              0x83, 0xb9, 0x58, 0x12, 0x00, 0x00, 0x11 };                 // "do_mm_skirmish"
        // beta26: push rbx / sub rsp,80h / cmp dword [rcx+1268h],11h / mov rbx,rcx
        static const unsigned char head26[] = { 0x40, 0x53, 0x48, 0x81, 0xec, 0x80, 0x00, 0x00, 0x00, 0x83, 0xb9, 0x68, 0x12, 0x00, 0x00, 0x11,
                                                0x48, 0x8b, 0xd9 };
        bool b26 = g_bi == BUILD_BETA26;
        ok = build_code("adv_opgame.codelib", skirmish_rva(), b26 ? head26 : head, b26 ? sizeof head26 : sizeof head) != 0;
    }
    return ok != 0;
}

// What the force picked on the Skirmish screen costs, priced as that screen prices it (its COST row): the
// player's picks are 0x84-byte entries at controller +0x1a74 (a counted template name, an attached vehicle
// at +0x40, experience at +0x80), as many as the battle size allows (6/10/14/18, table adv_opgame+0x134698,
// size at controller +0x131c), the two supports after them at +0xf78; adv_opgame+0x66650(controller, entry)
// gives one entry's cost. side: the screen's own side (controller +0x1950 -> +0x308).
// beta26: picks +0x1af4, size +0x139c, setup +0x19d0, price adv_opgame+0x725a0 (the same code; its own
// tables moved by +0x80 as well), table +0x15ec98 (the same six numbers 6, 10 .. 26).
static bool skirmish_cost(ObjMan *om, unsigned *cost, unsigned *side, unsigned *enemy = 0)
{
    static const unsigned char sig[] = { 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xec, 0x70, 0x66, 0x83, 0x3a, 0x00 };
    // beta26: push rbp / push r12 / push r14 / push r15 / sub rsp,78h / cmp word [rdx],0
    static const unsigned char sig26[] = { 0x40, 0x55, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xec, 0x78, 0x66, 0x83, 0x3a, 0x00 };
    static const unsigned table_sig[] = { 6, 10, 14, 18 };
    bool b26 = g_bi == BUILD_BETA26;
    char *fn = build_code("adv_opgame.codelib", BV(0x66650, 0x66350, 0x725a0), b26 ? sig26 : sig, b26 ? sizeof sig26 : sizeof sig);
    const unsigned *table = (const unsigned *)build_code("adv_opgame.codelib", BV(0x134698, 0x133698, 0x15ec98), table_sig, sizeof table_sig);
    char *c = (char *)menu_controller(om);
    if (!fn || !table || !c || !readable(c, mc_picks() + 0xf78 + 2 * 0x84)) return false;
    int size = *(int *)(c + mc_size());
    if (size < 0) size = 0;
    if (size > 3) size = 3;
    unsigned slots = table[size], total = 0;
    unsigned (*price)(void *, void *) = (unsigned (*)(void *, void *))fn;
    char *list = c + mc_picks();
    for (unsigned i = 0; i < slots && i < 30; i++) total += price(c, list + (size_t)i * 0x84);
    for (int i = 0; i < 2; i++) total += price(c, list + 0xf78 + (size_t)i * 0x84);
    char *spec = *(char **)(c + mc_spec());
    *cost = total;
    *side = readable(spec, 0x310) ? *(unsigned *)(spec + 0x308) : 0;
    if (enemy) *enemy = readable(spec, 0x310) ? *(unsigned *)(spec + 0x30c) : 0;
    return true;
}

static void open_stock_screen(ObjMan *om, int mode)
{
    unsigned char *base = (unsigned char *)build_module("adv_opgame.codelib");
    void *c = menu_controller(om);
    if (!base || !c) return;
    const char *name = mode == HOST_MODE_OPERATION ? "campaign" : mode == HOST_MODE_MISSION ? "missions" : "skirmish";
    if (mode == HOST_MODE_SKIRMISH) {
        if (!skirmish_stub_ok()) { mp_log("ui: unknown game build, open SKIRMISH yourself"); return; }
        ((void (*)(void *, u64, u64, char *))(base + skirmish_rva()))(c, 0, skirmish_event(), 0);
    } else {
        // "do_mm_campaign" (screen 2) and "do_mm_qbattle" (screen 3, the MISSIONS entry: prepared and
        // user-made missions) are stubs of the same shape: xor r8d,r8d / mov edx,<screen> / jmp switch
        // (beta26 +0x2b110 / +0x2b150, registered under those names in +0x2e5f0; their jumps, checked too
        // there, lead to its screen switch +0x847c0)
        size_t at = mode == HOST_MODE_OPERATION ? BV(0x2b210, 0x2b210, 0x2b110) : BV(0x2b250, 0x2b250, 0x2b150);
        const unsigned char stub[] = { 0x45, 0x33, 0xc0, 0xba, (unsigned char)(mode == HOST_MODE_OPERATION ? 2 : 3), 0x00, 0x00, 0x00, 0xe9,
                                       (unsigned char)(mode == HOST_MODE_OPERATION ? 0xa3 : 0x63), 0x96, 0x05, 0x00 };
        char *fn = build_code("adv_opgame.codelib", at, stub, g_bi == BUILD_BETA26 ? sizeof stub : 9);
        if (!fn) { mp_log("ui: unknown game build, open the %s screen yourself", name); return; }
        ((void (*)(void *))fn)(c);
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

// The 2026 beta can render through Direct3D 11, and does by default (config.cfgpack i_render u:render_api = 1):
// the object is then an i_render11 (basic_render vtable +0xeca00), which has no Direct3D 9 device at all -
// Direct3DCreate9 is called only by the Direct3D 9 i_render (render_api = 0, vtable +0xe7f50, set up in
// +0x4d290; its IDirect3D9 and device sit at +0x110/+0x118 as in the 2024 builds, so the walk below finds it).
// With Direct3D 11 the same drawing goes through ui_d3d11.cpp, on the engine's own Direct3D 11 device (see
// ui_render). Only if that cannot work here (no device found, no shader compiler) the log says so and the
// player is told once, in a message box, how to get the menu back.
static bool render_is_d3d11(Object *render)
{
    char *m = build_module("basic_render.codelib");
    size_t vt = BV(NP, NP, 0xeca00);
    return m && vt && *(char **)render == m + vt;
}

static DWORD WINAPI d3d11_notice(LPVOID)
{
    MessageBoxA(0, "Multiplayer cannot draw its menu with DirectX 11 here.\n\n"
                   "The game is set to DirectX 11, and the multiplayer menu could not be drawn with it on this PC (the log in "
                   "the game's out folder says why): the MULTIPLAYER entry of the main menu stays hidden.\n\n"
                   "Choose DirectX 9 in the game's graphics options and start the game again to host or join a game.",
                "Graviteam Tactics - Multiplayer", MB_OK | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND);
    return 0;
}

// Direct3D 11 drawing does not work in this game (ui11_begin said why in the log): the player is told once.
static void d3d11_unavailable(void)
{
    static bool told;
    if (told) return;
    told = true;
    mp_log("ui: the game renders with Direct3D 11 (graphics option DirectX 11, i_render u:render_api = 1) and the multiplayer "
           "menu, lobby and banners cannot be drawn with it here - they stay hidden; DirectX 9 in the game's graphics options brings them back");
    if (!g_tactical && !GetEnvironmentVariableA("MP_TEST", 0, 0))      // (tests read the log line)
        if (HANDLE t = CreateThread(0, 0, d3d11_notice, 0, 0, 0)) CloseHandle(t);
}

static IDirect3DDevice9 *find_device(ObjMan *om)
{
    static int tries;
    Object *render = engine_find(om, "i_render", "render");
    if (!render) { if (!tries++) mp_log("ui: no i_render object"); return 0; }
    if (g_dev && *(void **)((char *)render + g_dev_off) == g_dev_anchor) return g_dev;
    g_dev = 0;
    if (render_is_d3d11(render)) return 0;     // (no Direct3D 9 device: drawn through ui_d3d11.cpp, see ui_render)
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
// tex: an IDirect3DTexture9, or (g_d11) an image of ui_d3d11.cpp
struct TextTex { IUnknown *tex; int w, h; };
static std::unordered_map<std::string, TextTex> g_text;
static const void *g_text_dev;
static bool g_d11;                 // this frame is drawn through ui_d3d11.cpp (the game renders with Direct3D 11)

static void drop_text(void)
{
    for (auto &kv : g_text) if (kv.second.tex) kv.second.tex->Release();
    g_text.clear();
}

static TextTex *text_tex(IDirect3DDevice9 *dev, const char *s, int px, bool bold)
{
    const void *owner = g_d11 ? ui11_owner() : dev;
    if (owner != g_text_dev || g_text.size() > 96) { drop_text(); g_text_dev = owner; }
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
            IDirect3DTexture9 *tex9 = 0;
            if (g_d11) {
                // the same pixels as below, handed to the Direct3D 11 renderer
                std::vector<DWORD> img((size_t)w * h);
                for (int y = 0; y < h; y++) {
                    const unsigned char *src = (const unsigned char *)bits + (size_t)y * w * 4;
                    DWORD *dst = img.data() + (size_t)y * w;
                    for (int x = 0; x < w; x++) {
                        unsigned a = src[x * 4];
                        if (src[x * 4 + 1] > a) a = src[x * 4 + 1];
                        if (src[x * 4 + 2] > a) a = src[x * 4 + 2];
                        dst[x] = (a << 24) | 0xffffff;
                    }
                }
                if ((t.tex = ui11_texture(w, h, img.data())) != 0) {
                    t.w = w;
                    t.h = h;
                }
            } else if (SUCCEEDED(dev->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex9, 0))) {
                t.tex = tex9;
                D3DLOCKED_RECT lr;
                if (SUCCEEDED(tex9->LockRect(0, &lr, 0, 0))) {
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
                    tex9->UnlockRect(0);
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
    if (g_d11) { ui11_rect(x, y, w, h, color); return; }
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
    if (g_d11) { ui11_image(t->tex, x, y, w, h, color); return w; }
    Vtx v[4] = {
        { x - 0.5f, y - 0.5f, 0, 1, color, 0, 0 }, { x + w - 0.5f, y - 0.5f, 0, 1, color, 1, 0 },
        { x - 0.5f, y + h - 0.5f, 0, 1, color, 0, 1 }, { x + w - 0.5f, y + h - 0.5f, 0, 1, color, 1, 1 },
    };
    g_draw->SetTexture(0, static_cast<IDirect3DTexture9 *>(t->tex));
    g_draw->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    g_draw->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    g_draw->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    g_draw->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(Vtx));
    return w;
}

// The game's ESC menu is a column of picture buttons at the left edge (leave, controls, withdraw ...) that
// ends at about 0.65 of the screen's height; the CEASEFIRE button sits right under it.
static const float k_cease_left = 8, k_cease_y = 0.668f;
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
static unsigned port_of(const char *typed) { return typed[0] ? (unsigned)strtoul(typed, 0, 10) : k_default_port; }
static bool port_ok(const char *typed) { unsigned p = port_of(typed); return p >= 1 && p <= 65535; }

// A port as a field holds it: nothing for the default port or for something that is no port.
static void port_text(char *out, const char *in)
{
    unsigned p = (unsigned)strtoul(in, 0, 10);
    out[0] = 0;
    if (p >= 1 && p <= 65535 && p != k_default_port) sprintf_s(out, 8, "%u", p);
}

// "address" or "address:port" into the two fields of JOIN A GAME
static void split_target(const char *target)
{
    char buf[128];
    strncpy_s(buf, target, sizeof buf - 1);
    g_join_port[0] = 0;
    if (char *colon = strrchr(buf, ':')) {
        *colon = 0;
        port_text(g_join_port, colon + 1);
    }
    strncpy_s(g_address, buf, sizeof g_address - 1);
}

// ... and the two fields as the one address the connection is made to
static const char *join_target(void)
{
    static char target[96];
    sprintf_s(target, "%s:%u", g_address, port_of(g_join_port));
    return target;
}

// users\mp_last_host.txt: the host's address, the port joined on, the port hosted on, "upnp" if the box is
// ticked (a line each)
static void save_address(void)
{
    char path[MAX_PATH];
    sprintf_s(path, "%s\\users\\mp_last_host.txt", g_root);
    FILE *f = 0;
    if (!fopen_s(&f, path, "w") && f) { fprintf(f, "%s\n%s\n%s\n%s\n", g_address, g_join_port, g_host_port, g_upnp ? "upnp" : ""); fclose(f); }
}

static void load_address(void)
{
    char path[MAX_PATH], line[4][80] = {};
    sprintf_s(path, "%s\\users\\mp_last_host.txt", g_root);
    FILE *f = 0;
    if (!fopen_s(&f, path, "r") && f) {
        for (int i = 0; i < 4 && fgets(line[i], sizeof line[i], f); i++) line[i][strcspn(line[i], "\r\n")] = 0;
        fclose(f);
    }
    g_upnp = !strcmp(line[3], "upnp");
    split_target(line[0]);              // (an address saved by an earlier version carries its port)
    if (line[1][0]) port_text(g_join_port, line[1]);
    port_text(g_host_port, line[2]);
}

// Typing into the field that has the keyboard. digits: a port field - digits only, five at most.
static void type_field(char *buf, size_t room, bool digits)
{
    size_t n = strlen(buf);
    bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0, shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    auto add = [&](char c) { if (n + 1 < room) { buf[n++] = c; buf[n] = 0; } };
    for (int k = '0'; k <= '9'; k++) if (key_pressed(k) && !shift) add((char)k);
    for (int k = VK_NUMPAD0; k <= VK_NUMPAD9; k++) if (key_pressed(k)) add((char)('0' + k - VK_NUMPAD0));
    bool paste = false, colon = false;
    for (int k = 'A'; k <= 'Z'; k++)
        if (key_pressed(k)) {
            if (ctrl && k == 'V') paste = true;
            else if (!ctrl && !digits) add((char)(k + 32));
        }
    if ((key_pressed(VK_OEM_PERIOD) | key_pressed(VK_DECIMAL)) && !digits) add('.');
    if (key_pressed(VK_OEM_MINUS) && !digits) add('-');
    if (key_pressed(VK_OEM_1) && shift && !digits) colon = true;
    if (key_pressed(VK_BACK) && n) buf[--n] = 0;
    if (key_pressed(VK_DELETE)) buf[n = 0] = 0;
    if (colon) g_focus = 1;             // "address:port" typed in one go: the colon moves on to the port field
    if (paste && OpenClipboard(0)) {
        if (HANDLE h = GetClipboardData(CF_TEXT)) {
            if (const char *p = (const char *)GlobalLock(h)) {
                char clip[100];
                size_t m = 0;
                for (; *p && m + 1 < sizeof clip; p++)
                    if (digits ? isdigit((unsigned char)*p) != 0 : isalnum((unsigned char)*p) || *p == '.' || *p == ':' || *p == '-') clip[m++] = *p;
                clip[m] = 0;
                if (digits) strncpy_s(buf, room, clip, room - 1);
                else split_target(clip);            // a pasted "address:port" fills both fields
                GlobalUnlock(h);
            }
        }
        CloseClipboard();
    }
}

// A text field: what has been typed or, while it is empty, what that stands for, in grey. The field with
// the keyboard has a heavier frame and a blinking mark. True when it has just been clicked.
static bool field(float x, float y, float w, float h, const char *value, const char *ghost, bool keys)
{
    bool hot = g_mx >= x && g_mx < x + w && g_my >= y && g_my < y + h;
    quad(x, y, w, h, 0xfffffbf0);
    frame(x, y, w, h, (keys ? 3 : 2) * g_s, keys ? k_ink : k_edge);
    size_t len = strlen(value);
    float size = len > 34 ? 15.0f : len > 24 ? 20.0f : 26.0f, tx = x + 18 * g_s, ty = y + h / 2 - size * 0.81f * g_s;
    bool mark = keys && (GetTickCount() / 500) % 2;
    if (len) {
        char shown[80];
        sprintf_s(shown, "%s%s", value, mark ? "_" : "");
        text(shown, tx, ty, size, k_ink, 0);
    } else {
        if (ghost) text(ghost, tx, ty, size, 0xffa39a8c, 0);
        if (mark) text("_", tx, ty, size, k_ink, 0);
    }
    return hot && g_clicked;
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

// ---------------------------------------------------------------- CHOOSE MY FORCE
static void load_my_force(const char *path)
{
    std::vector<unsigned char> b;
    FILE *f = 0;
    if (fopen_s(&f, path, "rb") || !f) return;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n > 0 && n < (16 << 20)) { b.resize(n); b.resize(fread(b.data(), 1, b.size(), f)); }
    fclose(f);
    unsigned units = 0;
    g_my_force_side = force::player_side(b);
    force::count(b, g_my_force_side, &g_my_force_squads, &units);
    if (!g_my_force_squads) return;
    g_joiner.set_force(b);
    mp_log("ui: this player's own force is offered to the host: %u squads, %u units of the %s", g_my_force_squads, units, image::side_name(g_my_force_side));
}

static void drop_my_force(void)
{
    char path[MAX_PATH];
    sprintf_s(path, "%s\\users\\mp_my_force.cfgpack", g_root);
    DeleteFileA(path);
    g_joiner.set_force(std::vector<unsigned char>());
    g_my_force_squads = g_my_force_side = 0;
    g_force_picking = false;
    g_force_note[0] = 0;
}

// The Skirmish screen's start button writes the battle description and has base_shell start the battle
// program (CreateProcessA, caught in base_shell's import table). Picking a force, that description is this
// player's force: it is kept, and instead of the battle the menu program is started again, which goes back
// into the host's lobby by itself and offers the force (this program ends as it would for a battle).
typedef BOOL (WINAPI *CreateProcessFn)(LPCSTR, LPSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCSTR, LPSTARTUPINFOA, LPPROCESS_INFORMATION);
static CreateProcessFn g_real_create;

// The battle description a battle program is started on: the second item of its command line ("" none).
static void description_of(const char *cmd, char *rel, size_t size)
{
    rel[0] = 0;
    if (const char *comma = strchr(cmd, ',')) {
        const char *p = comma + 1;
        while (*p == ' ') p++;
        size_t n = strcspn(p, ",");
        while (n && p[n - 1] == ' ') n--;
        if (n < size) { memcpy(rel, p, n); rel[n] = 0; }
    }
}

static BOOL WINAPI start_battle_hook(LPCSTR app, LPSTR cmd, LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta, BOOL inherit, DWORD flags,
                                     LPVOID env, LPCSTR dir, LPSTARTUPINFOA si, LPPROCESS_INFORMATION pi)
{
    char rel[MAX_PATH] = "", path[MAX_PATH], keep[MAX_PATH], own[MAX_PATH + 8];
    // The host's skirmish or mission: the battle is made now, before its program starts (session_prepare_battle).
    if (!g_force_picking && cmd && strstr(cmd, "tacgame.progpack") && session_role() == ROLE_HOST && session_host_mode() != HOST_MODE_OPERATION) {
        description_of(cmd, rel, sizeof rel);
        sprintf_s(path, "%s\\%s", g_root, rel);
        if (rel[0]) session_prepare_battle(path);
        session_set_host_ready(false);
    }
    if (!g_force_picking || !cmd || !strstr(cmd, "tacgame.progpack")) return g_real_create(app, cmd, pa, ta, inherit, flags, env, dir, si, pi);
    session_restore_skirmish_setup(true);           // (the screen has just written its setup into the profile)
    description_of(cmd, rel, sizeof rel);
    sprintf_s(path, "%s\\%s", g_root, rel);
    sprintf_s(keep, "%s\\users\\mp_my_force.cfgpack", g_root);
    std::vector<unsigned char> b;
    FILE *f = 0;
    if (rel[0] && !fopen_s(&f, path, "rb") && f) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n > 0 && n < (16 << 20)) { b.resize(n); b.resize(fread(b.data(), 1, b.size(), f)); }
        fclose(f);
    }
    unsigned side = force::player_side(b), squads = 0, units = 0, want = g_joiner.force_side;
    force::count(b, side, &squads, &units);
    char note[200];
    if (!squads) {
        strcpy_s(note, "Your force could not be read from the Skirmish screen. Press CHOOSE MY FORCE to try again.");
    } else if (want && side != want) {
        sprintf_s(note, "That force is of the %s; you command the %s in this battle. CHOOSE MY FORCE again with the %s as your side.",
                  image::side_name(side), image::side_name(want), image::side_name(want));
    } else if (fopen_s(&f, keep, "wb") || !f) {
        strcpy_s(note, "Your force could not be kept (users folder).");
    } else {
        fwrite(b.data(), 1, b.size(), f);
        fclose(f);
        SetEnvironmentVariableA("MP_FORCE_FILE", keep);
        char cost[16];
        sprintf_s(cost, "%u", g_joiner.force_cost);
        SetEnvironmentVariableA("MP_FORCE_COST", cost);
        sprintf_s(note, "Your own force: %u squads of the %s, cost %u.", squads, image::side_name(side), g_joiner.force_cost);
    }
    mp_log("ui: CHOOSE MY FORCE - the Skirmish screen's battle is not started: %s Back to %s's lobby.", note, g_force_target);
    SetEnvironmentVariableA("MP_FORCE_NOTE", note);
    SetEnvironmentVariableA("MP_AUTOJOIN", g_force_target);
    SetEnvironmentVariableA("MP_AUTOJOIN_READY", "0");
    sprintf_s(own, "\"%s\\starter_x64.exe\"", g_root);
    return g_real_create(0, own, pa, ta, inherit, flags, env, g_root, si, pi);
}

// While picking, the Skirmish screen's start is let through only for a force the host's game can take: of
// the side this player commands, and costing no more than the host's own force.
static ObjMan *g_om_ui;
static DWORD g_setup_restore_at;                // CHOOSE MY FORCE: when the profile's own Skirmish setup goes back
static unsigned g_pick_cost, g_pick_side;
static bool g_pick_known, g_pick_date_wrong;     // g_pick_date_wrong: the screen's year or season is not the host's
static char g_pick_refusal[200];
typedef void (*SkirmishFn)(void *, u64, u64, char *);
static SkirmishFn g_skirmish;

// The host's start on its Skirmish screen: the host is ready (pressed again: not ready any more). The battle starts
// only when the joining player is ready too - at once if he already is, else from ui_frame when he becomes so.
static bool g_host_launching;
static bool hosting_skirmish(void) { return session_role() == ROLE_HOST && session_host_mode() == HOST_MODE_SKIRMISH; }

// beta26: the start this hook let through once and the game held back for its music fade, carried out now
// (controller +0x3dbc set by the fade tick, see skirmish_event): it was decided on then, so it goes straight on.
static bool deferred_start(void *c)
{
    size_t at = BV(NP, NP, 0x3dbc);
    return at && readable(c, at + 1) && *((unsigned char *)c + at) != 0;
}

static void skirmish_hook(void *c, u64 a, u64 b, char *d)
{
    if (deferred_start(c)) { mp_log("ui: the Skirmish screen's start, held back while the menu music faded out, goes on"); g_skirmish(c, a, b, d); return; }
    bool on_screen = readable(c, mc_screen() + 8) && *(int *)((char *)c + mc_screen()) == 0x11;
    if (!g_force_picking && !g_host_launching && hosting_skirmish() && on_screen) {
        if (session_host_ready()) { session_set_host_ready(false); return; }
        session_set_host_ready(true);
        if (!session_peer_ready_to_start()) return;
        mp_log("ui: both players are ready - the battle starts");
    }
    if (g_force_picking && g_om_ui && on_screen) {
        unsigned cost = 0, side = 0, limit = g_joiner.force_limit, want = g_joiner.force_side;
        bool priced = skirmish_cost(g_om_ui, &cost, &side);
        g_pick_refusal[0] = 0;
        if (!limit && g_joiner.host_in_battle) strcpy_s(g_pick_refusal, "The host's battle has begun - your own force can only go into the next one. Leave this screen and press READY to enter the battle.");
        else if (!limit) strcpy_s(g_pick_refusal, "The host has not chosen his force yet - its cost is your limit. Wait for it, then press start again.");
        else if (!priced) strcpy_s(g_pick_refusal, "Your force could not be priced.");
        else if (want && side != want) sprintf_s(g_pick_refusal, "You command the %s in this battle: set your own side to the %s.", image::side_name(want), image::side_name(want));
        else if (readable(c, mc_date() + 0x10) && ((g_joiner.force_year && *(unsigned *)((char *)c + mc_date()) + 1 != g_joiner.force_year) ||
                                                   (g_joiner.force_period && *(unsigned *)((char *)c + mc_date() + 4) + 1 != g_joiner.force_period)))
            strcpy_s(g_pick_refusal, "The battle is in the host's year and season: set YEAR AND SEASON back to what the screen opened with, then pick your units.");
        else if (cost > limit) sprintf_s(g_pick_refusal, "Your force costs %u - the host's costs %u. Take something out.", cost, limit);
        if (g_pick_refusal[0]) { mp_log("ui: CHOOSE MY FORCE - start refused: %s", g_pick_refusal); return; }
        g_joiner.force_cost = cost;
        mp_log("ui: CHOOSE MY FORCE - the force costs %u of %u allowed", cost, limit);
    }
    g_skirmish(c, a, b, d);
}

static void catch_battle_start(void)
{
    if (!g_skirmish && skirmish_stub_ok()) {
        char *at = build_module("adv_opgame.codelib") + skirmish_rva();
        // (2024: 5 + 5 + 1 + 4 bytes of whole instructions; beta26: 2 + 7 + 7, push / sub rsp / cmp [rcx+1268h])
        g_skirmish = (SkirmishFn)install_detour(at, BF(15, 16), skirmish_hook);
    }
    if (g_real_create) return;
    char *base = (char *)GetModuleHandleA("base_shell.codelib");
    if (!base) return;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY &imp = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!imp.VirtualAddress) return;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + imp.VirtualAddress); d->Name; d++) {
        if (_stricmp(base + d->Name, "KERNEL32.dll") || !d->OriginalFirstThunk) continue;
        IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(base + d->OriginalFirstThunk), *slots = (IMAGE_THUNK_DATA *)(base + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, slots++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            if (strcmp(((IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData))->Name, "CreateProcessA")) continue;
            DWORD old;
            if (!VirtualProtect(&slots->u1.Function, 8, PAGE_READWRITE, &old)) return;
            g_real_create = (CreateProcessFn)slots->u1.Function;
            slots->u1.Function = (ULONGLONG)start_battle_hook;
            VirtualProtect(&slots->u1.Function, 8, old, &old);
            return;
        }
    }
}

void ui_start(ObjMan *)
{
    g_tactical = !strcmp(g_program, "tacgame");
    strcpy_s(g_root, g_out_dir);
    if (char *s = strrchr(g_root, '\\')) *s = 0;
    load_address();
    joiner_steam_send() = steam_send;
    joiner_steam_recv() = steam_recv;
    g_joiner.game_build = g_bi;          // a host with another build of the game is not joined
    // Back from a joined battle: this game is an ordinary single-player game again until told otherwise.
    if (!g_tactical && session_role() == ROLE_JOIN && !session_map_mode()) session_single_player();
    if (!g_tactical && GetEnvironmentVariableA("MP_AUTOJOIN", g_autojoin, sizeof g_autojoin)) SetEnvironmentVariableA("MP_AUTOJOIN", 0);
    else g_autojoin[0] = 0;
    g_autojoin_ready = g_autojoin[0] != 0;
    char v[MAX_PATH];
    if (!g_tactical && GetEnvironmentVariableA("MP_AUTOJOIN_READY", v, sizeof v)) { if (!atoi(v)) g_autojoin_ready = false; SetEnvironmentVariableA("MP_AUTOJOIN_READY", 0); }
    if (!g_tactical && GetEnvironmentVariableA("MP_FORCE_NOTE", g_force_note, sizeof g_force_note)) SetEnvironmentVariableA("MP_FORCE_NOTE", 0);
    else g_force_note[0] = 0;
    // the force this player picked (just now, or before the last battle): offered to the host again
    char path[MAX_PATH];
    sprintf_s(path, "%s\\users\\mp_my_force.cfgpack", g_root);
    if (!g_tactical && (GetEnvironmentVariableA("MP_FORCE_FILE", v, sizeof v) || g_autojoin[0])) load_my_force(path);
    SetEnvironmentVariableA("MP_FORCE_FILE", 0);
    if (GetEnvironmentVariableA("MP_FORCE_COST", v, sizeof v)) g_joiner.force_cost = strtoul(v, 0, 10);       // (kept for the battles after)
    // Steam's "Join Game" on a friend who hosts: Steam may have started this game for it (the host's text is
    // then on the command line), or will tell it while it runs (steam.cpp). Steam is looked at now so that
    // its join request can arrive.
    if (!g_tactical) {
        steam_ready();
        if (strstr(GetCommandLineA(), "+gtmf_join=")) steam_join_from_text(GetCommandLineA(), 0);
    }
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

    if (g_tactical) {
        if (g_action == ACT_CEASEFIRE) endgame_toggle_truce();
        g_action = ACT_NONE;
        return;
    }
    g_on_main_menu = on_main_menu(om);
    g_om_ui = om;
    static DWORD priced_at;
    bool on_skirmish = menu_screen(om) == 0x11;
    if (!on_skirmish) g_pick_known = false;
    if (g_setup_restore_at && GetTickCount() >= g_setup_restore_at) { g_setup_restore_at = 0; session_restore_skirmish_setup(false); }
    if (GetTickCount() - priced_at >= 400 && on_skirmish) {
        priced_at = GetTickCount();
        unsigned cost = 0, side = 0, enemy = 0;
        bool ok = skirmish_cost(om, &cost, &side, &enemy);
        if (ok && session_role() == ROLE_HOST && session_host_mode() == HOST_MODE_SKIRMISH) {
            if (cost) session_set_force_limit(cost);
            if (side && enemy) session_set_setup_sides(side, enemy);
            char *c = (char *)menu_controller(om);
            if (readable(c, mc_date() + 0x10)) session_set_setup_date(*(unsigned *)(c + mc_date()), *(unsigned *)(c + mc_date() + 4));
        }
        if (g_force_picking) {
            g_pick_known = ok; g_pick_cost = cost; g_pick_side = side;
            char *c = (char *)menu_controller(om);
            g_pick_date_wrong = readable(c, mc_date() + 0x10) && ((g_joiner.force_year && *(unsigned *)(c + mc_date()) + 1 != g_joiner.force_year) ||
                                                                 (g_joiner.force_period && *(unsigned *)(c + mc_date() + 4) + 1 != g_joiner.force_period));
        }
    }
    g_host_on_skirmish = on_skirmish && hosting_skirmish();
    if (session_host_ready() && hosting_skirmish()) {
        if (!on_skirmish) session_set_host_ready(false);            // (left the Skirmish screen)
        else if (session_peer_ready_to_start() && g_skirmish && g_screen == SCR_NONE) {
            void *c = menu_controller(om);
            mp_log("ui: both players are ready - the battle starts");
            session_set_host_ready(false);          // (once: a start the screen turns down needs another press)
            g_host_launching = true;
            g_skirmish(c, 0, skirmish_event(), 0);
            g_host_launching = false;
        }
    }
    g_on_op_map = session_on_op_map(om);
    if (!g_on_main_menu || g_screen != SCR_NONE) g_button_hot = false;
    if (!g_on_op_map || g_screen != SCR_NONE) g_map_button_hot = false;
    // Steam's "Join Game": taken when this player is not hosting and not in a lobby; it waits for the main menu
    if (!g_autojoin[0] && session_role() != ROLE_HOST && (g_screen == SCR_NONE || g_screen == SCR_MAIN || g_screen == SCR_JOIN) &&
        steam_join_request(g_autojoin, sizeof g_autojoin)) {
        g_autojoin_ready = false;
        mp_log("ui: Steam's Join Game - into the lobby of %s when the main menu is up", g_autojoin);
    }
    // back from a turn on the operational map: into the host's lobby again, ready for the battles
    if (g_autojoin[0] && g_on_main_menu && g_frames > 45 && (g_screen == SCR_NONE || g_screen == SCR_MAIN || g_screen == SCR_JOIN)) {
        char target[128];
        if (!_strnicmp(g_autojoin, "steam:", 6)) {          // a host reached through Steam: no address fields
            strcpy_s(target, g_autojoin);
            strncpy_s(g_steam_host_name, steam_name_of(_strtoui64(g_autojoin + 6, 0, 10)), sizeof g_steam_host_name - 1);
        } else {
            split_target(g_autojoin);
            strcpy_s(target, join_target());
        }
        g_autojoin[0] = 0;
        g_message[0] = 0;
        mp_log("ui: rejoining the lobby of '%s'", target);
        g_joiner.prefer_side = 0;
        g_joiner.lobby_ready = g_autojoin_ready;
        g_joiner.swap_req = 0;
        strcpy_s(g_joiner.my_name, session_my_name());
        if (g_joiner.start(target)) { g_lobby_host = false; g_screen = SCR_LOBBY; }
        else { strcpy_s(g_message, g_joiner.error); g_screen = SCR_JOIN; }
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
        g_screen = hosting || g_force_picking ? SCR_LOBBY : SCR_MAIN;
        g_button_hot = false;
        mp_log("ui: multiplayer menu opened");
    } else if ((act == ACT_HOST_SKIRMISH || act == ACT_HOST_MISSION || act == ACT_HOST_OPERATION) && !port_ok(g_host_port)) {
        strcpy_s(g_message, "The port to host on has to be a number from 1 to 65535 (empty: 1943).");
    } else if (act == ACT_HOST_SKIRMISH || act == ACT_HOST_MISSION || act == ACT_HOST_OPERATION) {
        int mode = act == ACT_HOST_OPERATION ? HOST_MODE_OPERATION : act == ACT_HOST_MISSION ? HOST_MODE_MISSION : HOST_MODE_SKIRMISH;
        session_set_port((unsigned short)port_of(g_host_port));
        session_set_upnp(g_upnp);
        save_address();
        session_host(mode);
        if (mode != HOST_MODE_OPERATION) catch_battle_start();     // (the start held back, the battle made before it starts)
        mp_log("ui: hosting %s chosen, on UDP port %u, UPnP %s", mode == HOST_MODE_OPERATION ? "operation" : mode == HOST_MODE_MISSION ? "mission" : "skirmish",
               port_of(g_host_port), g_upnp ? "on" : "off");
        g_message[0] = 0;
        g_lobby_host = true;
        g_screen = SCR_LOBBY;
    } else if (act == ACT_SETUP && hosting) {
        if (session_host_mode() != HOST_MODE_OPERATION) catch_battle_start();
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
            drop_my_force();
            g_screen = SCR_JOIN;
        }
    } else if (act == ACT_TEST_SKIRMISH_START) {
        // tests: the stock Skirmish screen's start button (its handler starts the battle when that screen is up)
        if (menu_screen(om) == 0x11) { mp_log("test: the Skirmish screen's start"); open_stock_screen(om, HOST_MODE_SKIRMISH); }
        else mp_log("test: the Skirmish screen is not up (screen %d)", menu_screen(om));
    } else if (act == ACT_CHOOSE_FORCE && !g_lobby_host && g_screen == SCR_LOBBY && g_joiner.state == Joiner::WAITING_FOR_BATTLE) {
        g_force_picking = true;
        g_pick_refusal[0] = 0;
        g_pick_known = false;
        strcpy_s(g_force_target, g_joiner.address);
        g_joiner.lobby_ready = false;
        catch_battle_start();
        session_preset_skirmish(g_joiner.force_side, g_joiner.force_enemy, g_joiner.force_year, g_joiner.force_period);
        close_menu();
        open_stock_screen(om, HOST_MODE_SKIRMISH);
        g_setup_restore_at = GetTickCount() + 1500;     // (once the screen has read it)
        mp_log("ui: CHOOSE MY FORCE - Skirmish screen open; its start button hands this player's force to the host (%s)", g_force_target);
    } else if (act == ACT_CONNECT && g_address[0] && !port_ok(g_join_port)) {
        strcpy_s(g_message, "The port has to be a number from 1 to 65535 (empty: 1943).");
    } else if (act == ACT_CONNECT && g_address[0]) {
        save_address();
        g_message[0] = 0;
        mp_log("ui: connecting to '%s'", join_target());
        g_joiner.prefer_side = 0;
        g_joiner.lobby_ready = false;
        g_joiner.swap_req = 0;
        strcpy_s(g_joiner.my_name, session_my_name());
        if (g_joiner.start(join_target())) { g_lobby_host = false; g_screen = SCR_LOBBY; }
        else strcpy_s(g_message, g_joiner.error);
    } else if (act == ACT_BACK) {
        if (g_screen == SCR_JOIN) g_screen = SCR_MAIN;
        else close_menu();
    } else if (act == ACT_RESTORE && g_action_arg >= 0 && g_action_arg < (int)g_slots.size()) {
        restore_slot(g_slots[g_action_arg]);
    } else if (act == ACT_CONNECT_STEAM && g_action_arg >= 0 && g_action_arg < g_nshosts) {
        char target[40];
        sprintf_s(target, "steam:%llu", g_shosts[g_action_arg].id);
        strcpy_s(g_steam_host_name, g_shosts[g_action_arg].name);
        g_message[0] = 0;
        mp_log("ui: joining %s through Steam ('%s')", g_steam_host_name, target);
        g_joiner.prefer_side = 0;
        g_joiner.lobby_ready = false;
        g_joiner.swap_req = 0;
        strcpy_s(g_joiner.my_name, session_my_name());
        if (g_joiner.start(target)) { g_lobby_host = false; g_screen = SCR_LOBBY; }
        else strcpy_s(g_message, g_joiner.error);
    }

    // F9: the lobby while hosting (the host is usually on the game's Skirmish or Operations screen then)
    if (focused && key_pressed(VK_F9) && g_screen == SCR_NONE && (hosting || g_on_main_menu)) g_action = ACT_OPEN;
    if (g_force_picking && g_screen == SCR_NONE && !g_lobby_host) g_joiner.poll();       // (the host keeps this player meanwhile)
    if (g_screen == SCR_NONE) return;

    bool escape = focused && key_pressed(VK_ESCAPE);
    if (g_screen == SCR_JOIN) {
        if (GetTickCount() - g_shosts_at >= 1000) {         // the friends who host, looked up once a second
            g_shosts_at = GetTickCount();
            g_nshosts = steam_hosts(g_shosts, _countof(g_shosts));
        }
        if (focused && key_pressed(VK_TAB)) g_focus ^= 1;
        if (focused && g_focus) type_field(g_join_port, 6, true);
        else if (focused) type_field(g_address, sizeof g_address, false);
        if (focused && key_pressed(VK_RETURN)) g_action = ACT_CONNECT;
        if (escape) g_action = ACT_BACK;
    } else if (g_screen == SCR_MAIN) {
        if (focused) type_field(g_host_port, 6, true);         // the only field of this panel
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
    if (g_lobby_host || peer_here) text(operation ? "Operation" : mission ? "Custom mission" : "Skirmish", cx, py + 88 * s, 22, k_edge);
    if (g_lobby_host) {
        // what a player outside this network needs: the public address, and the port reaching this PC
        int upnp = session_upnp();
        const char *pub = session_public_ip();
        if (upnp == 2) sprintf_s(line, "UPnP: the router passes UDP port %u on to this PC.%s%s", session_port(), pub[0] ? "  Your public address: " : "", pub);
        else if (upnp == 1) sprintf_s(line, "UPnP: asking the router to pass UDP port %u on to this PC ...", session_port());
        else if (upnp == 3) sprintf_s(line, "UPnP: the router did not open the port - forward UDP port %u to this PC by hand.", session_port());
        else sprintf_s(line, "UPnP is off - players outside your network need UDP port %u forwarded to this PC.", session_port());
        text(line, cx, py + 121 * s, 14, upnp == 2 ? good : upnp == 3 ? wait : k_edge);
    }

    // the two players: name, the force each commands, state
    float nx = px + 60 * s, fx = px + 300 * s, sx = px + pw - 60 * s, y1 = py + 150 * s, y2 = py + 204 * s;
    const char *host_force = operation ? "the side of the operation" : mission ? (swap ? "the mission's ENEMY force" : "the mission's PLAYER force")
                           : swap ? "ENEMY force of the battle setup" : "OWN force of the battle setup";
    const char *join_force = operation ? "the opposing side" : mission ? (swap ? "the mission's PLAYER force" : "the mission's ENEMY force")
                           : swap ? "OWN force of the battle setup" : "ENEMY force of the battle setup";
    char own_force[96];
    unsigned his_squads = 0, his_side = 0;
    if (!operation && !mission && g_lobby_host && session_peer_force(&his_squads, &his_side)) {
        if (session_peer_force_cost()) sprintf_s(own_force, "his own force: %u squads of the %s, cost %u", his_squads, image::side_name(his_side), session_peer_force_cost());
        else sprintf_s(own_force, "his own force: %u squads of the %s", his_squads, image::side_name(his_side));
        join_force = own_force;
    } else if (!operation && !mission && !g_lobby_host && g_my_force_squads && g_joiner.force_have == g_joiner.force_hash) {
        sprintf_s(own_force, "his own force: %u squads of the %s", g_my_force_squads, image::side_name(g_my_force_side));
        join_force = own_force;
    }
    if (host_name) {
        sprintf_s(line, "%s  (host)", host_name);
        text(line, nx, y1, 22, k_ink, 0, true);
        text(host_force, fx, y1 + 3 * s, 17, k_ink, 0);
        if (!g_lobby_host) text(g_joiner.host_in_battle ? "in the battle" : g_joiner.host_ready && !operation && !mission ? "READY" : "setting up", sx, y1 + 3 * s, 17,
                                g_joiner.host_in_battle || (g_joiner.host_ready && !operation && !mission) ? good : k_edge, 2);
        else if (!operation && !mission) text(session_host_ready() ? "READY" : "setting up", sx, y1 + 3 * s, 17, session_host_ready() ? good : k_edge, 2);
    } else {
        if (g_joiner.steam) sprintf_s(line, "Looking for %s through Steam ...  (%u s)", g_steam_host_name[0] ? g_steam_host_name : "the host", g_joiner.waited_seconds());
        else sprintf_s(line, "Looking for the host at %s ...  (%u s)", g_joiner.address, g_joiner.waited_seconds());
        text(line, nx, y1, 20, wait, 0);
    }
    if (join_name) {
        text(join_name, nx, y2, 22, k_ink, 0, true);
        text(join_force, fx, y2 + 3 * s, 17, k_ink, 0);
        text(peer_ready ? "READY" : "not ready", sx, y2 + 3 * s, 17, peer_ready ? good : wait, 2);
    } else {
        sprintf_s(line, "Waiting for a player to join at  %s : %u", session_local_ip()[0] == '?' ? "your address" : session_local_ip(), session_port());
        text(line, nx, y2 - 4 * s, 20, wait, 0);
        if (steam_ready()) text("Your Steam friends find this game on their JOIN A GAME screen - they need no address and no open port.", nx, y2 + 25 * s, 14, k_edge, 0);
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
        text("The host sets up the battle on the Skirmish screen. The other player may pick his own force with CHOOSE MY FORCE;", cx, y, 15, k_edge);
        text("otherwise he commands the one the setup gives him. SWAP SIDES exchanges who commands which.", cx, y + 24 * s, 15, k_edge);
        unsigned limit = g_lobby_host ? session_force_limit() : peer_here ? g_joiner.force_limit : 0;
        if (g_lobby_host) {
            if (limit) sprintf_s(line, "Your force costs %u: the other player's own force may cost as much.", limit);
            else strcpy_s(line, "Choose your force on the Skirmish screen: what it costs is the most the other player's own force may cost.");
            text(line, cx, y + 62 * s, 17, limit ? good : wait);
            text("The Skirmish screen's start makes you ready; the battle starts when the other player is ready too.", cx, y + 92 * s, 14, k_edge);
        }
        if (!g_lobby_host && peer_here && !starting) {
            // where this player's own force stands
            unsigned want = g_joiner.force_side;
            if (g_my_force_squads) {
                const char *state = g_joiner.force_have == g_joiner.force_hash ? "the host has it."
                                  : g_joiner.force_refused == g_joiner.force_hash ? "the host cannot use it - it is not of your side."
                                  : "being sent to the host...";
                sprintf_s(line, "Your own force: %u squads of the %s - %s", g_my_force_squads, image::side_name(g_my_force_side), state);
                text(line, cx, y + 62 * s, 17, g_joiner.force_have == g_joiner.force_hash ? good : wait);
            } else if (g_force_note[0]) {
                text(g_force_note, cx, y + 62 * s, 17, wait);
            }
            if (want) {
                if (limit) sprintf_s(line, "You command the %s. CHOOSE MY FORCE opens the Skirmish screen: the %s as your side, units costing up to %u (the host's force), then its start button.",
                                     image::side_name(want), image::side_name(want), limit);
                else sprintf_s(line, "You command the %s. Your own force may cost as much as the host's - he has not chosen his yet.", image::side_name(want));
                if (limit && g_joiner.host_in_battle)
                    sprintf_s(line, "The host's battle is still in deployment: CHOOSE MY FORCE (the %s, up to %u) puts your own force in - his battle starts again with it.",
                              image::side_name(want), limit);
                text(line, cx, y + 92 * s, 14, k_edge);
            }
        }
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
            bool choose = !operation && !mission && peer_here;
            if (operation || !peer_here) {
                if (button(g_joiner.lobby_ready ? "NOT READY" : "READY", cx, r1, bw, bh)) g_action = ACT_READY;
            } else {
                if (button(g_joiner.lobby_ready ? "NOT READY" : "READY", left, r1, hw, bh)) g_action = ACT_READY;
                if (button("SWAP SIDES", right, r1, hw, bh)) g_action = ACT_SWAP;
            }
            if (choose && button("CHOOSE MY FORCE", cx, r2, bw, bh)) g_action = ACT_CHOOSE_FORCE;
            char no_answer[120];
            if (g_joiner.steam) strcpy_s(no_answer, "No answer yet. Steam is looking for a way to the host's game; that can take a moment.");
            else sprintf_s(no_answer, "No answer yet. Check the address and the port, and that UDP port %u reaches the host.", port_of(g_join_port));
            const char *hint = !peer_here ? (g_joiner.waited_seconds() >= 8 ? no_answer : "")
                             : !g_joiner.lobby_ready ? (g_joiner.host_in_battle ? "The host is in the battle. Press READY to enter it."
                                                        : !operation && !mission ? (g_joiner.host_ready ? "The host is ready. Press READY when you are - the battle starts then."
                                                                                                         : "Press READY when you are; the battle starts when both players are ready.")
                                                        : "Press READY when you are; the battle starts when the host starts it.")
                             : !operation && !mission && !g_joiner.host_in_battle ? (g_joiner.host_ready ? "Ready. The battle is starting..." : "Ready. Waiting for the host to press start on his Skirmish screen...")
                             : "Ready. Waiting for the host to start the battle...";
            text(hint, cx, choose ? r1 - 40 * s : r2 + 22 * s, 17, g_joiner.lobby_ready ? good : wait);
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
        if (button("JOIN A GAME", cx, y + 3 * gap, bw, bh)) { g_message[0] = 0; g_focus = 0; g_screen = SCR_JOIN; }
        if (button("BACK", cx, y + 4 * gap, bw, bh)) g_action = ACT_BACK;
        // for hosting: the port the game listens on (typing goes here on this panel), and whether the router
        // is asked to pass that port on to this PC
        float x0 = cx - bw / 2, fh = 50 * s, fy = py + 644 * s, fw = 120 * s, fx = x0 + 172 * s;
        text("Host port (UDP)", x0, fy + fh / 2 - 16 * s, 19, k_ink, 0);
        field(fx, fy, fw, fh, g_host_port, "1943", true);
        float bx = x0 + 322 * s, bs = 34 * s, by = fy + (fh - bs) / 2;
        bool box_hot = g_mx >= bx && g_mx < x0 + bw && g_my >= by && g_my < by + bs;
        quad(bx, by, bs, bs, box_hot ? k_paper_hot : 0xfffffbf0);
        frame(bx, by, bs, bs, 2 * s, k_edge);
        if (g_upnp) text("X", bx + bs / 2, by + bs / 2 - 19 * s, 24, k_ink, 1, true);
        text("Open it by UPnP", bx + bs + 12 * s, fy + fh / 2 - 16 * s, 19, k_ink, 0);
        if (box_hot && g_clicked) { g_upnp = !g_upnp; save_address(); mp_log("ui: UPnP %s", g_upnp ? "on" : "off"); }
        text(g_message, cx, py + ph - 34 * s, 15, 0xff8a1c10);
    } else if (g_screen == SCR_JOIN) {
        text("JOIN A GAME", cx, py + 34 * s, 40, k_ink, 1, true);
        // two fields in the row of the old one: the host's address, and its port (empty: 1943, shown in grey)
        float fh = 70 * s, fy = y + 6 * s, aw = 400 * s, qw = 140 * s, ax = cx - bw / 2, qx = cx + bw / 2 - qw;
        text("Host address (type it, or Ctrl+V to paste)", ax, py + 112 * s, 18, k_edge, 0);
        text("Port", qx, py + 112 * s, 18, k_edge, 0);
        if (field(ax, fy, aw, fh, g_address, 0, g_focus == 0)) g_focus = 0;
        text(":", (ax + aw + qx) / 2, fy + fh / 2 - 21 * s, 26, k_ink, 1);
        if (field(qx, fy, qw, fh, g_join_port, "1943", g_focus == 1)) g_focus = 1;
        if (button("CONNECT", cx, y + gap, bw, bh)) g_action = ACT_CONNECT;
        if (button("BACK", cx, y + 2 * gap, bw, bh)) g_action = ACT_BACK;
        // ... or without an address: the Steam friends who are hosting a game right now
        float ly = py + 448 * s;
        quad(ax, ly - 10 * s, bw, 2 * s, k_edge);
        text("STEAM FRIENDS HOSTING A GAME", ax, ly, 17, k_ink, 0, true);
        if (!steam_ready()) {
            text("Steam is not available to this game.", ax, ly + 32 * s, 15, k_edge, 0);
        } else if (!g_nshosts) {
            text("None of your Steam friends is hosting right now.", ax, ly + 32 * s, 15, k_edge, 0);
            text("A friend who hosts shows up here; joining him needs no address and no port.", ax, ly + 54 * s, 14, k_edge, 0);
        }
        for (int i = 0; i < g_nshosts; i++) {
            float ry = ly + 34 * s + i * 44 * s;
            char what[64];
            sprintf_s(what, "hosts %s %s", !strcmp(g_shosts[i].what, "operation") ? "an" : "a", !strcmp(g_shosts[i].what, "mission") ? "custom mission" : g_shosts[i].what);
            text(g_shosts[i].name, ax, ry, 20, k_ink, 0, true);
            text(what, ax + 250 * s, ry + 4 * s, 15, k_edge, 0);
            if (button("JOIN", cx + bw / 2 - 60 * s, ry - 3 * s, 120 * s, 36 * s, 16)) { g_action = ACT_CONNECT_STEAM; g_action_arg = i; }
        }
        text(g_message, cx, py + ph - 60 * s, 17, 0xff8a1c10);
    } else if (g_screen == SCR_MAPWAIT) {
        draw_map_wait(cx, py, bw, bh);
    } else {
        draw_lobby(cx, px, py, pw, ph);
    }
}

// ---------------------------------------------------------------- the ESC menu's CEASEFIRE button
// Drawn with the game's ESC menu while a battle with another player is running. A press is this player's
// word for a ceasefire (endgame.cpp), a second press takes it back; with both players' word the battle ends.
static void draw_ceasefire(void)
{
    int truce = g_tactical && session_menu_open() ? endgame_truce() : 0;
    g_cease_hot = false;
    if (!truce) return;
    char peer[24] = "the other player", label[96], note[160];
    session_peer(peer, sizeof peer, 0);
    if ((truce & 3) == 3) {
        strcpy_s(label, "CEASEFIRE AGREED");
        sprintf_s(note, "The battle ends a few seconds after the menu is closed. Press again to take your word back.");
    } else if (truce & 1) {
        strcpy_s(label, "CEASEFIRE PROPOSED");
        sprintf_s(note, "Waiting for %s to choose CEASEFIRE as well. Press again to take the proposal back.", peer);
    } else if (truce & 2) {
        strcpy_s(label, "ACCEPT CEASEFIRE");
        sprintf_s(note, "%s proposes a ceasefire. The battle ends when you accept.", peer);
    } else {
        strcpy_s(label, "CEASEFIRE");
        sprintf_s(note, "Propose to end the battle now. It ends when %s chooses CEASEFIRE too.", peer);
    }
    float w = 250 * g_s, h = 40 * g_s, x = k_cease_left * g_s, y = g_h * k_cease_y;
    g_cease_hot = g_mx >= x && g_mx < x + w && g_my >= y && g_my < y + h;
    static int tests = -1;
    if (tests < 0) tests = GetEnvironmentVariableA("MP_TEST", 0, 0) != 0;
    if (tests && g_clicked) mp_log("ui: click at %.0f, %.0f with the ESC menu open; CEASEFIRE is at %.0f..%.0f, %.0f..%.0f", g_mx, g_my, x, x + w, y, y + h);
    if (button(label, x + w / 2, y, w, h, 19)) g_action = ACT_CEASEFIRE;
    if (!g_cease_hot) return;           // what it does is said while the mouse is on it, like the menu's own buttons
    TextTex *t = text_tex(g_draw, note, (int)(15 * g_s + 0.5f), false);
    float nw = (t ? t->w : 0) + 24 * g_s;
    quad(x, y + h + 6 * g_s, nw, 28 * g_s, 0xc0181410);
    text(note, x + 12 * g_s, y + h + 8 * g_s, 15, 0xfff2e6c8, 0);
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
    if (!strncmp(line, "withdraw", 8)) { session_test_withdraw(); return false; }
    if (!strncmp(line, "pause", 5)) { session_test_pause(); return false; }
    if (!strncmp(line, "speed ", 6)) { session_test_speed(atoi(line + 6)); return false; }
    if (!strncmp(line, "time", 4)) { session_test_time(); return false; }
    if (!strncmp(line, "ceasefire", 9)) { g_action = ACT_CEASEFIRE; return false; }
    if (!strncmp(line, "chooseforce", 11)) { g_action = ACT_CHOOSE_FORCE; return false; }
    // tests that must not depend on the mouse (the player may have other windows over the game)
    if (!strncmp(line, "hostskirmish", 12)) { g_action = ACT_HOST_SKIRMISH; return false; }
    if (!strncmp(line, "setup", 5)) { g_action = ACT_SETUP; return false; }
    if (!strncmp(line, "lobbyready", 10)) { g_action = ACT_READY; return false; }
    if (!strncmp(line, "skirmishstart", 13)) { g_action = ACT_TEST_SKIRMISH_START; return false; }
    if (!strncmp(line, "connect ", 8)) {
        char target[128] = "";
        if (sscanf_s(line + 8, "%127s", target, (unsigned)sizeof target) == 1) { split_target(target); g_action = ACT_CONNECT; }
        return false;
    }
    if (!strncmp(line, "steamrequest ", 13)) { char text[128]; steam_connect_text(text, sizeof text, _strtoui64(line + 13, 0, 10)); steam_join_from_text(text, 1); return false; }
    if (!strncmp(line, "steamjoin", 9)) { g_action = ACT_CONNECT_STEAM; g_action_arg = 0; return false; }
    if (!strncmp(line, "menuoff", 7)) { session_test_menu(false); return false; }
    if (!strncmp(line, "menu", 4)) { session_test_menu(true); return false; }
    if (!strncmp(line, "endturn", 7)) { session_test_next_turn(); return false; }
    if (!strncmp(line, "savemap", 7)) { session_test_save(); return false; }
    if (!strncmp(line, "newmap", 6)) { session_test_new_map(); return false; }
    if (!strncmp(line, "flares", 6)) { shots_test_flares(atoi(line + 6)); return false; }
    if (!strncmp(line, "badnode", 7)) { state_test_guard(); return false; }
    if (!strncmp(line, "order ", 6)) { int n = 0; float dx = 0, dz = 0; if (sscanf_s(line + 6, "%d %f %f", &n, &dx, &dz) >= 2) orders_test_move(n, dx, dz); return false; }
    if (!strncmp(line, "attack", 6)) { orders_test_attack(); return false; }
    if (!strncmp(line, "selectable", 10)) { orders_test_selectable(); return false; }
    if (!strncmp(line, "aim ", 4)) { int s = 0, n = 0; float dx = 0, dz = 0, deg = 40; if (sscanf_s(line + 4, "%d %d %f %f %f", &s, &n, &dx, &dz, &deg) >= 2) orders_test_aim(s, n, dx, dz, deg); return false; }
    if (!strncmp(line, "place ", 6)) { int n = 0; float dx = 0, dz = 0; if (sscanf_s(line + 6, "%d %f %f", &n, &dx, &dz) == 3) orders_test_place(n, dx, dz); return false; }
    if (!strncmp(line, "squadpos ", 9)) { orders_test_where(strtoul(line + 9, 0, 16)); return false; }
    if (!strncmp(line, "seats", 5)) { seats_test_list(); return false; }
    if (!strncmp(line, "unseat ", 7)) { seats_test_move(atoi(line + 7)); return false; }
    if (!strncmp(line, "reseat", 6)) { seats_test_move(-1); return false; }
    if (!strncmp(line, "cells", 5)) { cells_test_list(); return false; }
    if (!strncmp(line, "capture ", 8)) { cells_test_capture(atoi(line + 8)); return false; }
    if (!strncmp(line, "blowup", 6)) { shots_test_blowup(atoi(line + 6)); return false; }
    if (!strncmp(line, "artillery", 9)) { int r = 0, k = 0; sscanf_s(line + 9, "%d %d", &r, &k); shots_test_artillery(r, k); return false; }
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

// Over the Skirmish screen while this player picks his own force: what it costs against the host's, and why
// the start button did nothing if it did nothing.
static void draw_pick_banner(void)
{
    unsigned limit = g_joiner.force_limit, want = g_joiner.force_side;
    char line[240];
    bool over = !limit || g_pick_cost > limit, wrong = (want && g_pick_side && g_pick_side != want) || g_pick_date_wrong;
    if (limit) sprintf_s(line, "MULTIPLAYER - your own force: cost %u of %u allowed (what the host's force costs)%s%s%s", g_pick_cost, limit,
                         want && g_pick_side && g_pick_side != want ? " - set your own side to the " : "",
                         want && g_pick_side && g_pick_side != want ? image::side_name(want) : "",
                         g_pick_date_wrong ? " - set YEAR AND SEASON back to the host's" : "");
    else if (g_joiner.host_in_battle) strcpy_s(line, "MULTIPLAYER - the host's battle has begun: your own force can only go into the next one");
    else strcpy_s(line, "MULTIPLAYER - the host has not chosen his force yet: its cost will be your limit");
    TextTex *t = text_tex(g_draw, line, (int)(18 * g_s + 0.5f), false);
    // (at the foot of the screen: the top line is where the Skirmish screen shows its own messages)
    float w = (t ? t->w : 0) + 40 * g_s, h = g_pick_refusal[0] ? 64 * g_s : 36 * g_s, y = g_h - h - 60 * g_s;
    quad(g_w / 2 - w / 2, y, w, h, 0xd0181410);
    text(line, g_w / 2, y + 4 * g_s, 18, over || wrong ? 0xffff8a70 : 0xffa8f0a0);
    if (g_pick_refusal[0]) text(g_pick_refusal, g_w / 2, y + 34 * g_s, 16, 0xffff8a70);
}

// Over the host's Skirmish screen: whether the battle waits for the other player, and for what.
static void draw_host_banner(void)
{
    char line[240], peer[24];
    bool peer_ready = false, here = session_peer(peer, sizeof peer, &peer_ready), ready = session_host_ready();
    if (!here) strcpy_s(line, ready ? "MULTIPLAYER - you are ready: the battle starts when a player has joined and is ready (start again: not ready)"
                                    : "MULTIPLAYER - nobody has joined yet: press start when you are ready, the battle starts when the other player is too");
    else if (ready && peer_ready) sprintf_s(line, "MULTIPLAYER - you and %s are ready: his own force is arriving, then the battle starts", peer);
    else if (ready) sprintf_s(line, "MULTIPLAYER - you are ready: the battle starts when %s is ready (start again: not ready)", peer);
    else sprintf_s(line, "MULTIPLAYER - %s is %s: press start when you are ready, the battle starts when both players are", peer, peer_ready ? "READY" : "not ready yet");
    TextTex *t = text_tex(g_draw, line, (int)(18 * g_s + 0.5f), false);
    float w = (t ? t->w : 0) + 40 * g_s, h = 36 * g_s, y = g_h - h - 60 * g_s;
    quad(g_w / 2 - w / 2, y, w, h, 0xd0181410);
    text(line, g_w / 2, y + 4 * g_s, 18, ready ? 0xffa8f0a0 : 0xfff2e6c8);
}

// What a frame shows, drawn with either renderer.
static void draw_overlays(const char *status, bool hint, bool picking, bool host_wait)
{
    if (g_screen != SCR_NONE) {
        draw_menu();
    } else {
        if (hint) draw_main_menu_button();
        if (picking) draw_pick_banner();
        if (host_wait) draw_host_banner();

        if (status[0]) {
            TextTex *t = text_tex(g_draw, status, (int)(17 * g_s + 0.5f), false);
            float w = (t ? t->w : 0) + 36 * g_s, h = 34 * g_s;
            quad(g_w / 2 - w / 2, 46 * g_s, w, h, 0xc0181410);
            text(status, g_w / 2, 46 * g_s + 3 * g_s, 17, session_connected() ? 0xffa8f0a0 : 0xfff2e6c8);
        }
        draw_ceasefire();
    }
}

// save_frame for the Direct3D 11 back buffer (the same file; the log gives its DXGI format).
static void save_frame11(const char *name)
{
    char path[MAX_PATH];
    unsigned w = 0, h = 0, format = 0;
    sprintf_s(path, "%s\\%s.bmp", g_out_dir, name);
    bool ok = ui11_save_frame(path, &w, &h, &format);
    mp_log("test: frame %s %s (%ux%u, format %u)", name, ok ? "saved" : "could not be captured", w, h, format);
}

// The game renders with Direct3D 11: the same frame through ui_d3d11.cpp, onto the back buffer, the size of
// the back buffer taking the place of the Direct3D 9 viewport's.
static void render_d3d11(Object *render, const char *status, bool hint, bool picking, bool host_wait, const char *shot)
{
    float w = 0, h = 0;
    int ok = ui11_begin(render, &w, &h);
    if (ok < 0) d3d11_unavailable();
    if (ok <= 0) return;
    g_w = w;
    g_h = h;
    g_s = g_h / 1080.0f;
    g_draw = 0;
    g_d11 = true;
    draw_overlays(status, hint, picking, host_wait);
    ui11_flush();
    if (shot[0]) save_frame11(shot);
    ui11_end();
    g_d11 = false;
    g_clicked = false;
}

void ui_render(ObjMan *om)
{
    const char *status = session_status();
    bool hint = !g_tactical && g_screen == SCR_NONE && g_on_main_menu;        // the main menu's MULTIPLAYER entry
    bool picking = !g_tactical && g_screen == SCR_NONE && g_force_picking && g_pick_known;     // CHOOSE MY FORCE: the Skirmish screen
    bool host_wait = !g_tactical && g_screen == SCR_NONE && g_host_on_skirmish && !g_force_picking;   // the host's Skirmish screen
    char shot[64] = "";
    test_shot_request(shot, sizeof shot);
    if (g_screen == SCR_NONE && !status[0] && !hint && !shot[0] && !g_on_op_map && !picking && !host_wait) {
        if (!g_text.empty() && g_frames % 600 == 0) drop_text();       // nothing shown: hold no device resources
        if (g_frames % 600 == 0) ui11_idle();
        return;
    }
    if (g_bi == BUILD_BETA26)          // (the only build with a Direct3D 11 renderer)
        if (Object *render = engine_find(om, "i_render", "render"))
            if (render_is_d3d11(render)) {
                static int off = -1;       // tests: MP_UI_D3D11=0 draws nothing on Direct3D 11 (to tell the menu's drawing from the game's)
                if (off < 0) { char v[8]; off = GetEnvironmentVariableA("MP_UI_D3D11", v, sizeof v) && !atoi(v); }
                if (!off) render_d3d11(render, status, hint, picking, host_wait, shot);
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

    draw_overlays(status, hint, picking, host_wait);

    if (shot[0]) save_frame(dev, shot);
    if (began) dev->EndScene();
    saved->Apply();
    saved->Release();
    g_clicked = false;
}
