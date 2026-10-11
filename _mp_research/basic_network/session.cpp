// Network session: one host, one joining player, UDP.
//
// The role comes from the in-game multiplayer menu (ui.cpp) or, for scripts and tests, from the environment:
//   MP_ROLE=host | join     MP_PEER=<address>[:port]     MP_PORT=<port>
//   MP_HOST_MODE=skirmish | operation                     MP_PREFER=GER | USSR (side the joining player wants)
// The environment is also how the role travels between the game's two programs: the operational program
// starts the tactical one as a child process, which inherits it.
//
// The host runs the battle. While it is in its menus it only tells callers to wait. Once a battle runs it
// serves the battle description, the battle clock and phase, and the state of the units it owns.
// Ownership: in a versus game each player's game owns the units that player commands and the other game
// follows them; when both look from the host's side the host owns everything. For owned units a game sends
// transforms about 15 times a second (this file) and the full saved state when it changes (statesync.cpp).
#include "common.h"
#include "protocol.h"
#include "image.h"
#include "detour.h"
#include "opsave.h"
#include "joiner.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <natupnp.h>
#include <oleauto.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

static const int k_units_per_packet = 14;       // 24 + 14 * 80 = 1144 bytes, below any sane MTU

static int g_role;
static int g_host_mode;
static bool g_tactical, g_versus;
static unsigned g_prefer_side;
static SOCKET g_sock = INVALID_SOCKET;
static sockaddr_in g_peer;
static bool g_have_peer, g_connected, g_bind_pending, g_upnp_started;
static unsigned short g_port = k_default_port;
static DWORD g_last_rx, g_last_hello, g_last_snapshot, g_last_report, g_last_bind_try;
static char g_root[MAX_PATH], g_local_ip[64] = "?", g_external_ip[64], g_status[320];
// lobby: who is playing, and whether the two players have exchanged forces (skirmish only)
static char g_my_name[24] = "Player", g_peer_name[24], g_image_file[MAX_PATH];
static bool g_peer_lobby_ready, g_swap, g_peer_swap_known;
// Operation: the operational map is with one player at a time. It travels as the operation's save.
static char g_profile_id[64] = "cloud";
static std::vector<unsigned char> g_map_out, g_map_in, g_map_have;   // save offered to the other game / being received
static std::vector<DWORD> g_map_asked;
static u32 g_map_out_hash, g_map_in_hash, g_map_got, g_map_ack, g_map_turns;
static char g_map_file[96];             // the save's file name, e.g. 1942_07_Oskol_07_02_odintsov_rus.opsave
static bool g_map_given;                // host: the joining player has a copy of the map; joining game: sending it back
static bool g_map_mode;                 // this program is the joining player's side of the operational map
static bool g_map_fetched;              // host: the joining game has started downloading the copy
static bool g_map_collect;              // host: ending the turn, the joining game's moves are being fetched
static bool g_collect_asked;            // joining game: the host asked for the moves
static bool g_end_turn_pending;         // host: restarted on the merged save, the turn still has to be ended
static DWORD g_map_collect_since, g_share_after, g_map_seen_since;
static DWORD g_turn_fix_at;             // host: the turn has been processed, the command tags go back at this time
static DWORD g_map_restart_at, g_map_leave_at;
static opsave::Sidecar g_map_sidecar;
static char g_map_note[120];            // last thing that happened, for the banner
static bool g_turn_done, g_peer_turn_done;  // operational map: this player / the other one has pressed "next turn"
static bool g_peer_on_map;              // host: the joining game is on its copy of the map
static bool g_map_sent;                 // joining game: the host has this player's moves for the turn
// Which state of the operation a copy of the map is of: the host counts up whenever the operation moves on,
// a joining game remembers the number of its own copy.
static u32 g_map_epoch, g_map_base_epoch, g_map_in_epoch, g_map_in_turns;
static char g_map_in_file[96];
static DWORD g_map_reload_at, g_map_battle_at;
static unsigned char g_peer_swap_req;

// battle description: served by the host, read by both for sides and unit ownership
static std::vector<unsigned char> g_image;
static u32 g_image_hash, g_user_side, g_enemy_side;
static std::unordered_set<u64> g_my_units;          // versus: the units this game's player commands
static std::unordered_set<u64> g_position_only;     // units whose saved state is not exchanged

// unit transforms
struct Sent { float pos[3], rot0[3]; DWORD when; };
static std::unordered_map<u64, Sent> g_sent;            // what was last sent per owned unit
static std::unordered_map<u64, Object *> g_units;       // local unit per id
static u32 g_units_count_seen;
static std::vector<UnitRec> g_pending;                  // received, applied before the next simulation step
// The last two transforms received per followed unit, with the time each arrived: the unit is shown
// between them, a little in the past (see session_sim_step).
struct Snap { UnitRec rec; double at; };
struct Held { Snap a, b; int have; };
static std::unordered_map<u64, Held> g_held;
static float g_host_speed = 1, g_host_cur_speed = 1;
static u32 g_host_sim_ms;
static float g_clock_factor = 1;
static unsigned char g_host_paused;
static bool g_have_host_clock;
static u32 g_peer_sim_ms, g_peer_phase;    // host: the joining game's battle clock and phase, and when they were heard
static DWORD g_peer_clock_at, g_waiting_since;
static bool g_clock_adopted;
static u32 g_snapshot, g_stat_sent, g_stat_applied, g_stat_packets, g_stat_bytes, g_stat_snaps;
static double g_stat_err_sum;
static float g_stat_err_max;
static u64 g_sim_ms;
static u32 g_phase, g_host_phase;          // battle phase here / on the host
static DWORD g_last_phase_step;
static DWORD g_host_lost;                  // joining game: when the host went away (0 = it has not)
// Both players have to press Start to leave deployment, and again to leave the initial orders.
static u32 g_my_ready, g_peer_ready;       // the phase each player has asked to move on to (1 or 2)
typedef u64 (*StartAction)(Object *, u64, u64, u64);
typedef void (*InputAction)(Object *, u64, const char *);
typedef void (*BoxResult)(Object *, int, u64, u64);
typedef void (*TimerStep)(char *, float, u32);
static StartAction g_start_action;         // the engine's own functions, reached past our detours
static InputAction g_input_action;
static BoxResult g_box_result;
static TimerStep g_timer_step;
typedef void (*SetSpeed)(char *, float);
typedef void (*SetPause)(char *, unsigned char);
static SetSpeed g_set_speed;               // the timer's own setters, replaced on the timer object of a joining game
static SetPause g_set_pause;
static double g_sim_step = 0.02;           // multiplayer battles simulate in steps of this many seconds (MP_SIM_HZ)

int session_role(void) { return g_role; }
int session_host_mode(void) { return g_host_mode; }
bool session_connected(void) { return g_connected; }
bool session_versus(void) { return g_versus; }
const char *session_local_ip(void) { return g_local_ip; }
unsigned short session_port(void) { return g_port; }
// load measurement for the 5 s report
static u32 g_prof_frames, g_prof_steps, g_prof_step_ms;
static double g_prof_save, g_prof_load;
static double seconds_now(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / f.QuadPart;
}

void session_note_sim_ms(u32 ms)
{
    if (g_phase == 2) g_sim_ms += ms;      // battle time
    g_prof_steps += ms != 0;
    g_prof_step_ms += ms;
}

void session_note_phase(u32 phase)
{
    static const char *names[] = { "deployment", "initial orders", "battle", "battle over" };
    g_phase = phase;
    mp_log("phase: %s", phase < 4 ? names[phase] : "?");
}

const char *session_my_name(void) { return g_my_name; }
bool session_swap(void) { return g_swap; }

void session_toggle_swap(void)
{
    g_swap = !g_swap;
    SetEnvironmentVariableA("MP_SWAP", g_swap ? "1" : "0");
    mp_log("lobby: sides swapped - the host commands the %s force of the battle setup", g_swap ? "enemy" : "own");
}

// The other player, if one is connected. ready: pressed READY in the lobby.
bool session_peer(char *name, size_t size, bool *ready)
{
    if (!g_connected) return false;
    strcpy_s(name, size, g_peer_name[0] ? g_peer_name : "player");
    if (ready) *ready = g_peer_lobby_ready;
    return true;
}

// One line for the on-screen banner; empty when this game is not in multiplayer. Always the same shape:
//     MULTIPLAYER  -  <who is playing>  -  <what is going on>
const char *session_status(void)
{
    g_status[0] = 0;
    if (g_map_mode) {
        const char *host = g_peer_name[0] ? g_peer_name : "the host";
        char what[200];
        if (!g_connected) sprintf_s(what, "connecting to %s", host);
        else if (g_map_battle_at) sprintf_s(what, "%s has started the battle - entering it", host);
        else if (g_map_sent && !g_map_in.empty()) sprintf_s(what, "the new turn is arriving");
        else if (g_map_sent) sprintf_s(what, "the turn is being carried out - the map reloads with the result (orders given now do not count)");
        else if (g_map_given) sprintf_s(what, "both players have ended the turn - sending your moves");
        else if (g_turn_done) sprintf_s(what, "you have ended the turn - waiting for %s  (next turn again: go on giving orders)", host);
        else if (g_peer_turn_done) sprintf_s(what, "%s has ended his turn - press next turn when your orders are given", host);
        else sprintf_s(what, "move your own forces; the turn ends when both players have pressed next turn");
        sprintf_s(g_status, "MULTIPLAYER  -  %s on the operational map  -  %s", g_my_name, what);
        return g_status;
    }
    if (g_role == ROLE_NONE || (g_role == ROLE_JOIN && !g_tactical)) return g_status;
    const char *peer = g_peer_name[0] ? g_peer_name : "the other player";
    char who[140], state[150];
    if (!g_tactical) {
        sprintf_s(who, "%s is hosting %s", g_my_name, g_host_mode == HOST_MODE_OPERATION ? "an operation" :
                  g_host_mode == HOST_MODE_MISSION ? "a custom mission" : "a skirmish");
    } else if (g_connected || g_peer_name[0]) {
        char mine[16], theirs[16];
        strcpy_s(mine, image::side_name(g_user_side));
        strcpy_s(theirs, image::side_name(g_enemy_side));
        if (g_versus || g_role == ROLE_HOST) sprintf_s(who, "%s (%s)  vs  %s (%s)", g_my_name, mine, peer, theirs);
        else sprintf_s(who, "%s watching %s (%s)", g_my_name, peer, mine);
    } else {
        sprintf_s(who, "%s (%s)", g_my_name, image::side_name(g_user_side));
    }

    const char *what = g_phase == 0 ? "deployment" : "the initial orders";
    bool mine = g_my_ready > g_phase, theirs = g_peer_ready > g_phase;
    if (g_role == ROLE_HOST && (g_bind_pending || g_sock == INVALID_SOCKET))
        sprintf_s(state, "UDP port %u is busy, retrying", g_port);
    else if (g_role == ROLE_JOIN && g_host_lost)
        sprintf_s(state, "%s has left, returning to the menu", peer);
    else if (!g_connected && g_role == ROLE_HOST)
        sprintf_s(state, "waiting for a player at %s:%u", g_local_ip, g_port);
    else if (!g_connected)
        sprintf_s(state, "connecting to the host");
    else if (!g_tactical && (g_map_collect || g_map_restart_at || g_end_turn_pending))
        sprintf_s(state, "ending the turn with %s's moves", peer);
    else if (!g_tactical && g_map_given && g_peer_on_map && g_turn_done)
        sprintf_s(state, "you have ended the turn - waiting for %s to finish his orders  (next turn again: take it back)", peer);
    else if (!g_tactical && g_map_given && g_peer_on_map && g_peer_turn_done)
        sprintf_s(state, "%s has finished his orders - press next turn when yours are given", peer);
    else if (!g_tactical && g_map_given && g_peer_on_map)
        sprintf_s(state, "%s is moving his forces on his own map - the turn ends when both have pressed next turn", peer);
    else if (!g_tactical && g_map_given)
        sprintf_s(state, "%s is getting his copy of the map", peer);
    else if (!g_tactical && g_map_note[0])
        sprintf_s(state, "%s", g_map_note);
    else if (!g_tactical)
        sprintf_s(state, "%s has joined and is %s  (F9: lobby)", peer, g_peer_lobby_ready ? "ready" : "not ready yet");
    else if (g_phase < 2 && g_start_action && mine && !theirs)
        sprintf_s(state, "waiting for %s to finish %s", peer, what);
    else if (g_phase < 2 && g_start_action && theirs && !mine)
        sprintf_s(state, "%s has finished %s, press Start when ready", peer, what);
    else if (g_phase < 2 && g_start_action && !mine)
        sprintf_s(state, "%s ends when both players press Start", g_phase == 0 ? "deployment" : "initial orders");
    else if (g_phase < 2)
        sprintf_s(state, "connected, starting");
    else if (g_phase == 2 && g_waiting_since && GetTickCount() - g_waiting_since > 1500)
        sprintf_s(state, "waiting for %s's game to catch up", peer);
    else if (g_phase == 2 && g_role == ROLE_JOIN && g_clock_factor > 1.5f)
        sprintf_s(state, "catching up with %s's game", peer);
    else if (g_phase == 2)
        sprintf_s(state, "connected");
    else
        sprintf_s(state, "battle over");
    sprintf_s(g_status, "MULTIPLAYER  -  %s  -  %s", who, state);
    return g_status;
}

static void send_to(const sockaddr_in &to, const void *data, int size)
{
    sendto(g_sock, (const char *)data, size, 0, (const sockaddr *)&to, sizeof to);
    g_stat_bytes += size;
}

static void send_peer(const void *data, int size)
{
    if (g_sock != INVALID_SOCKET && g_have_peer) send_to(g_peer, data, size);
}

// Full-state replication. MP_SYNC_STATE=0 switches it off (positions only), =1 forces it on.
// In operation battles it is off unless forced: there a unit's saved state refers to the formations of the
// game that saved it, and loading it into the other game crashes that game's unit update (seen in every
// operation battle tried; skirmishes and missions, with one group per side, are not affected).
static bool sync_state_enabled(void)
{
    static int forced = -2;
    if (forced == -2) {
        char v[8];
        forced = GetEnvironmentVariableA("MP_SYNC_STATE", v, sizeof v) ? (atoi(v) != 0) : -1;
    }
    return forced >= 0 ? forced != 0 : g_host_mode != HOST_MODE_OPERATION;
}

static bool unit_key(Object *o, u64 *key, UnitRec *rec)
{
    unsigned char kind = !strcmp(o->iface, "i_techn") ? 1 : !strcmp(o->iface, "i_human") ? 2 : 0;
    if (!kind) return false;
    const char *us = strrchr(o->full, '_');
    if (!us) return false;
    u32 id = strtoul(us + 1, 0, 16);
    *key = ((u64)kind << 32) | id;
    if (rec) { rec->id = id; rec->kind = kind; }
    return true;
}

// Some vehicle models do not survive having another game's saved state loaded into them (the game
// crashes in its renderer shortly afterwards): supply trucks, seen with opel_blitz_36 in operation battles.
// They are followed by position only. MP_STATE_SKIP lists the model-name fragments (comma separated).
static void find_position_only_units(void)
{
    char list[256] = "opel,zis5,zis_5,gaz_aa,gaz_mm,truck";
    GetEnvironmentVariableA("MP_STATE_SKIP", list, sizeof list);
    g_position_only.clear();
    std::unordered_map<u64, std::string> models = image::unit_models(g_image);
    for (auto &kv : models) {
        char *ctx = 0, copy[256];
        strcpy_s(copy, list);
        for (char *tok = strtok_s(copy, ", ", &ctx); tok; tok = strtok_s(0, ", ", &ctx))
            if (kv.second.find(tok) != std::string::npos) { g_position_only.insert(kv.first); break; }
    }
    if (!g_position_only.empty()) mp_log("net: %zu units are followed by position only (%s)", g_position_only.size(), list);
}

bool session_state_wanted(Object *o)
{
    u64 key;
    if (g_position_only.empty() || strcmp(o->iface, "i_techn") || !o->full) return true;
    const char *us = strrchr(o->full, '_');
    if (!us) return true;
    key = (1ull << 32) | strtoul(us + 1, 0, 16);
    return g_position_only.count(key) == 0;
}

// Which game an object's state comes from.
static bool owns_object(Object *o)
{
    u64 key;
    if (!g_versus || !unit_key(o, &key, 0)) return g_role == ROLE_HOST;
    return g_my_units.count(key) != 0;
}

bool session_owns(Object *o) { return owns_object(o); }
u32 session_phase(void) { return g_phase; }

// A squad is its units: a unit's id is its squad's id plus its place in it (the low five bits).
bool session_owns_squad(u32 id)
{
    if (!g_versus) return g_role == ROLE_HOST;
    static std::unordered_set<u32> squads;
    static size_t built_from;
    if (built_from != g_my_units.size()) {
        squads.clear();
        for (u64 key : g_my_units) squads.insert((u32)key & 0x7fffffe0);
        built_from = g_my_units.size();
    }
    return squads.count(id & 0x7fffffe0) != 0;
}
bool session_unit_key(Object *o, u64 *key) { return unit_key(o, key, 0); }
u64 session_battle_ms(void) { return g_sim_ms; }

// ---------------------------------------------------------------- battle description
static bool read_file(const char *path, std::vector<unsigned char> &out)
{
    FILE *f = 0;
    if (fopen_s(&f, path, "rb") || !f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? n : 0);
    size_t got = n > 0 ? fread(out.data(), 1, n, f) : 0;
    fclose(f);
    return got == out.size() && n > 0;
}

static void load_battle_description(void)
{
    // the description this battle was started from: second item on the command line
    char path[MAX_PATH], rel[MAX_PATH];
    strcpy_s(rel, k_image_path);
    if (const char *comma = strchr(GetCommandLineA(), ',')) {
        const char *p = comma + 1;
        while (*p == ' ') p++;
        size_t n = strcspn(p, ",");
        if (n && n < sizeof rel) { memcpy(rel, p, n); rel[n] = 0; }
    }
    sprintf_s(path, "%s\\%s", g_root, rel);
    strcpy_s(g_image_file, path);
    if (!read_file(path, g_image)) { mp_log("net: cannot read the battle description %s", path); return; }
    g_image_hash = fnv(g_image.data(), g_image.size());
    image::sides(g_image, &g_user_side, &g_enemy_side);
    // In this game's own description the squads tagged USER are the ones its player commands.
    g_my_units = image::units_of(g_image, image::k_user);
    find_position_only_units();
    mp_log("net: battle description %zu bytes (hash %08x), this player commands %s (%zu units)", g_image.size(), g_image_hash,
           image::side_name(g_user_side), g_my_units.size());
}

// ---------------------------------------------------------------- versus: no computer commander
// In a versus game both forces have a human commander. The engine's AI commander (i_aicolonel) would keep
// giving orders to the "enemy" force of each game, fighting the other player's orders, so its simulation
// step is switched off in this process: the first instruction of its handler becomes a return.
// A host keeps it off from the start of its battle, so the opponent has no computer-made plan when the other
// player takes command; it comes back if the battle begins without a second player, or with one on the host's side.
static void update_ai_commander(void)
{
    static const unsigned char sig[] = { 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0x8b, 0xf9 };
    static int state = -1;          // -1 unknown, 0 active, 1 off, 2 unknown game build
    if (!g_tactical || g_role == ROLE_NONE || state == 2) return;
    bool off = g_versus || (g_role == ROLE_HOST && !g_connected && g_phase < 2);
    if (state == (int)off) return;
    HMODULE m = GetModuleHandleA("adv_game.codelib");
    if (!m) return;
    unsigned char *fn = (unsigned char *)m + 0xbec30;
    if ((*fn != 0x40 && *fn != 0xc3) || memcmp(fn + 1, sig, sizeof sig)) {
        state = 2;
        mp_log("net: unknown game build, the AI commander stays active");
        return;
    }
    DWORD old;
    if (!VirtualProtect(fn, 1, PAGE_EXECUTE_READWRITE, &old)) return;
    *fn = off ? 0xc3 : 0x40;
    VirtualProtect(fn, 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), fn, 1);
    state = off;
    mp_log("net: AI commander switched %s", off ? "off" : "on");
}

static void set_versus(bool versus)
{
    if (versus == g_versus) return;
    g_versus = versus;
    mp_log("net: %s", versus ? "versus: each game owns its own player's units" : "same side: the host owns every unit");
    g_sent.clear();
    statesync_reset();
}

// ---------------------------------------------------------------- addresses, UPnP (host)
static void find_local_ip(void)
{
    // the address the default route uses; connect() on a UDP socket sends nothing
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in probe = {};
    probe.sin_family = AF_INET;
    probe.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &probe.sin_addr);
    if (connect(s, (sockaddr *)&probe, sizeof probe) == 0) {
        sockaddr_in me;
        int n = sizeof me;
        if (getsockname(s, (sockaddr *)&me, &n) == 0) inet_ntop(AF_INET, &me.sin_addr, g_local_ip, sizeof g_local_ip);
    }
    closesocket(s);
}

static DWORD WINAPI upnp_thread(LPVOID)
{
    if (FAILED(CoInitializeEx(0, COINIT_MULTITHREADED))) return 0;
    IUPnPNAT *nat = 0;
    IStaticPortMappingCollection *maps = 0;
    HRESULT hr = CoCreateInstance(__uuidof(UPnPNAT), 0, CLSCTX_ALL, __uuidof(IUPnPNAT), (void **)&nat);
    if (SUCCEEDED(hr)) hr = nat->get_StaticPortMappingCollection(&maps);
    if (FAILED(hr) || !maps) {
        mp_log("net: UPnP not available on this network (0x%08lx); internet play needs UDP %u forwarded by hand", hr, g_port);
    } else {
        wchar_t wip[64];
        MultiByteToWideChar(CP_ACP, 0, g_local_ip, -1, wip, 64);
        BSTR proto = SysAllocString(L"UDP"), client = SysAllocString(wip), desc = SysAllocString(L"Graviteam Tactics multiplayer");
        IStaticPortMapping *m = 0;
        hr = maps->Add(g_port, proto, g_port, client, VARIANT_TRUE, desc, &m);
        if (SUCCEEDED(hr)) {
            BSTR ext = 0;
            if (m && SUCCEEDED(m->get_ExternalIPAddress(&ext)) && ext) {
                WideCharToMultiByte(CP_ACP, 0, ext, -1, g_external_ip, sizeof g_external_ip, 0, 0);
                SysFreeString(ext);
            }
            mp_log("net: UPnP mapped UDP %u (external address %s)", g_port, g_external_ip[0] ? g_external_ip : "unknown");
        } else {
            mp_log("net: UPnP mapping refused (0x%08lx); relying on a manual port forward of UDP %u", hr, g_port);
        }
        if (m) m->Release();
        SysFreeString(proto); SysFreeString(client); SysFreeString(desc);
        maps->Release();
    }
    if (nat) nat->Release();
    CoUninitialize();
    return 0;
}

// ---------------------------------------------------------------- start / stop
static bool open_socket(unsigned short port)
{
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    u_long nonblocking = 1;
    ioctlsocket(g_sock, FIONBIO, &nonblocking);
    DWORD off = 0, ret = 0;
    WSAIoctl(g_sock, _WSAIOW(IOC_VENDOR, 12) /* SIO_UDP_CONNRESET */, &off, sizeof off, 0, 0, &ret, 0, 0);
    int buf = 1 << 21;
    setsockopt(g_sock, SOL_SOCKET, SO_RCVBUF, (char *)&buf, sizeof buf);
    setsockopt(g_sock, SOL_SOCKET, SO_SNDBUF, (char *)&buf, sizeof buf);
    sockaddr_in me = {};
    me.sin_family = AF_INET;
    me.sin_addr.s_addr = INADDR_ANY;
    me.sin_port = htons(port);
    if (bind(g_sock, (sockaddr *)&me, sizeof me)) {
        closesocket(g_sock);
        g_sock = INVALID_SOCKET;
        return false;
    }
    return true;
}

static void host_listen(void)
{
    // The operational program hands the port over to the tactical one; for a moment both may exist.
    g_last_bind_try = GetTickCount();
    if (!open_socket(g_port)) {
        if (!g_bind_pending) mp_log("net: UDP port %u is still in use, retrying", g_port);
        g_bind_pending = true;
        return;
    }
    g_bind_pending = false;
    mp_log("net: hosting %son %s UDP %u", g_tactical ? "battle " : "", g_local_ip, g_port);
    if (!g_upnp_started) {
        g_upnp_started = true;
        CloseHandle(CreateThread(0, 0, upnp_thread, 0, 0, 0));
    }
}

// Become the host now (multiplayer menu). mode: HOST_MODE_SKIRMISH or HOST_MODE_OPERATION.
void session_host(int mode)
{
    session_stop();
    g_role = ROLE_HOST;
    g_host_mode = mode;
    SetEnvironmentVariableA("MP_ROLE", "host");
    SetEnvironmentVariableA("MP_HOST_MODE", mode == HOST_MODE_OPERATION ? "operation" : mode == HOST_MODE_MISSION ? "mission" : "skirmish");
    g_swap = false;
    SetEnvironmentVariableA("MP_SWAP", "0");
    SetEnvironmentVariableA("MP_PEER_NAME", 0);
    g_peer_name[0] = 0;
    find_local_ip();
    host_listen();
}

// Back to single player (multiplayer menu).
void session_single_player(void)
{
    session_stop();
    g_role = ROLE_NONE;
    SetEnvironmentVariableA("MP_ROLE", 0);
    SetEnvironmentVariableA("MP_PEER", 0);
    SetEnvironmentVariableA("MP_VERSUS", 0);
    SetEnvironmentVariableA("MP_PREFER", 0);
}

// ---------------------------------------------------------------- lobby decisions carried into the battle
static void read_profile_name(void)
{
    char path[MAX_PATH], text[512] = "";
    sprintf_s(path, "%s\\users\\profiles\\cur_profile.engcfg", g_root);
    FILE *f = 0;
    if (fopen_s(&f, path, "rb") || !f) return;
    text[fread(text, 1, sizeof text - 1, f)] = 0;
    fclose(f);
    const char *p = strstr(text, "cur_profile");
    if (p && (p = strchr(p, '='))) {
        for (p++; *p == ' ' || *p == '\t'; p++) {}
        size_t k = 0;
        while (k < sizeof g_my_name - 1 && (isalnum((unsigned char)p[k]) || p[k] == '_' || p[k] == '-')) k++;
        if (k) { memcpy(g_my_name, p, k); g_my_name[k] = 0; }
    }
    p = strstr(text, "cur_id");
    if (p && (p = strchr(p, '='))) {
        for (p++; *p == ' ' || *p == '\t'; p++) {}
        size_t k = strcspn(p, ";\r\n");
        if (k && k < sizeof g_profile_id) { memcpy(g_profile_id, p, k); g_profile_id[k] = 0; }
    }
}

// Skirmish or mission, sides swapped in the lobby: the host plays the force its battle setup calls the enemy. The
// battle description is turned round on disk and this battle is started again from it; everything else
// (what the joining game downloads, who owns which unit) then follows from the description as usual.
static void host_swap_sides_if_asked(void)
{
    char v[8];
    if (g_host_mode == HOST_MODE_OPERATION || !g_swap || g_image.empty()) return;      // skirmishes and missions
    if (GetEnvironmentVariableA("MP_SWAPPED", v, sizeof v)) return;         // this is the restarted battle
    std::vector<unsigned char> turned = g_image;
    if (!image::mirror(turned)) { mp_log("lobby: this battle cannot be swapped, playing it as set up"); return; }
    FILE *f = 0;
    if (fopen_s(&f, g_image_file, "wb") || !f) { mp_log("lobby: cannot rewrite the battle description, playing it as set up"); return; }
    fwrite(turned.data(), 1, turned.size(), f);
    fclose(f);
    SetEnvironmentVariableA("MP_SWAPPED", "1");
    std::string cmd = GetCommandLineA();
    STARTUPINFOA si = { sizeof si };
    PROCESS_INFORMATION pi;
    if (CreateProcessA(0, &cmd[0], 0, 0, FALSE, 0, 0, g_root, &si, &pi)) {
        mp_log("lobby: sides swapped, restarting the battle from the turned description");
        ExitProcess(0);
    }
    mp_log("lobby: could not restart the battle (%lu), playing it as set up", GetLastError());
    if (!fopen_s(&f, g_image_file, "wb") && f) { fwrite(g_image.data(), 1, g_image.size(), f); fclose(f); }
    SetEnvironmentVariableA("MP_SWAPPED", 0);
}

// Operation: the engine saves the operation at the moment it starts a battle. That save (every operation
// save of the profile, with the terrain damage files) is copied to users\mp_saves\<host>_vs_<player>, so
// the host keeps the state "after the move, before the battle" whatever happens to either game afterwards.
static int copy_matching(const char *from, const char *to, const char *pattern)
{
    char find[MAX_PATH], src[MAX_PATH], dst[MAX_PATH];
    WIN32_FIND_DATAA fd;
    int n = 0;
    sprintf_s(find, "%s\\%s", from, pattern);
    HANDLE h = FindFirstFileA(find, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        sprintf_s(src, "%s\\%s", from, fd.cFileName);
        sprintf_s(dst, "%s\\%s", to, fd.cFileName);
        if (CopyFileA(src, dst, FALSE)) n++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return n;
}

static void snapshot_operation_saves(void)
{
    if (g_host_mode != HOST_MODE_OPERATION) return;
    char id[64] = "cloud", peer[24] = "nobody", from[MAX_PATH], to[MAX_PATH];
    const char *p = GetCommandLineA();
    for (int item = 0; item < 3 && p; item++) p = strchr(p, ',') ? strchr(p, ',') + 1 : 0;      // 4th item: profile id
    if (p) {
        while (*p == ' ') p++;
        size_t n = strcspn(p, ",");
        if (n && n < sizeof id) { memcpy(id, p, n); id[n] = 0; }
    }
    GetEnvironmentVariableA("MP_PEER_NAME", peer, sizeof peer);
    sprintf_s(from, "%s\\users\\profiles\\%s", g_root, id);
    sprintf_s(to, "%s\\users\\mp_saves", g_root);
    CreateDirectoryA(to, 0);
    sprintf_s(to, "%s\\users\\mp_saves\\%s_vs_%s", g_root, g_my_name, peer);
    CreateDirectoryA(to, 0);
    int n = copy_matching(from, to, "*.opsave") + copy_matching(from, to, "*.opsave_") + copy_matching(from, to, "*.damsav2");
    mp_log("lobby: operation saved before the battle: %d files in users\\mp_saves\\%s_vs_%s", n, g_my_name, peer);
}

// ---------------------------------------------------------------- operational map: each player moves his own side
// On the operational map the game only accepts moves for the formations tagged 'USER' in the operation's
// save, and it has no notion of a second player. So each player gets his own copy of the operation:
//   1. When the host is on the map and the other player is ready in the lobby, the game's own save routine
//      writes the operation (adv_opgame+0xd6660), the command tags are turned round (opsave.h) and the
//      joining game downloads the result.
//   2. The joining game starts the operation program on that save - as the same operation played from the
//      other side when the game has one, which gives its player his own territory and fog of war - and
//      both players move their own forces at the same time, each on his own map.
//   3. The host ends the turn. That is intercepted: the joining game saves and sends its map, the host
//      saves its own, the other player's battle groups are copied from his save into the host's
//      (opsave::merge_moves), and the host's operation program is restarted on the result and ends the
//      turn there. The battles that follow are fought by both as usual; the joining game is back in the
//      lobby by then and is taken into them.
static void *menu_controller_of(ObjMan *om)
{
    Object *og = engine_find(om, "i_opgame", "opgame");
    if (!og) return 0;
    void *c = *(void **)((char *)og + 0x100);
    return readable(c, 0x1400) ? c : 0;
}

// The operation object while its map is on screen: the controller is in operation mode (+0x1200 == 0; it
// is 4 in the menus) and holds the operation at +0x1d8. The screen number (+0x1258) is 0 when the program
// was started straight on the operation (as after a battle) and stays 2, the Operations screen's, when
// the player opened the operation from that screen.
static void *operation_on_screen(ObjMan *om)
{
    if (g_tactical) return 0;
    char *c = (char *)menu_controller_of(om);
    if (!c || (*(int *)(c + 0x1258) != 0 && *(int *)(c + 0x1258) != 2) || *(int *)(c + 0x1200) != 0) return 0;
    void *op = *(void **)(c + 0x1d8);
    return readable(op, 0x100) ? op : 0;
}

bool session_on_op_map(ObjMan *om) { return operation_on_screen(om) != 0; }
bool session_map_given(void) { return g_map_given; }
bool session_map_mode(void) { return g_map_mode; }
bool session_map_arriving(void) { return g_map_collect || g_map_restart_at != 0 || g_end_turn_pending || g_turn_fix_at != 0; }
// The moments in which the map must not take clicks: a joining game while its moves travel to the host;
// the host from the moment they are fetched until the turn has been carried out and who-commands-what is
// back in place (about twenty seconds). In that stretch both sides are player-led in the host's game: a
// battle started then would be made from that state - the host commanding both sides. The map itself
// stays in view, with the moves being made; nothing is drawn over it.
bool session_map_blocking(void)
{
    if (g_map_mode) return g_map_given && !g_map_sent;
    return g_map_collect || g_map_restart_at != 0 || g_end_turn_pending || g_turn_fix_at != 0;
}

static void profile_path(char *out, size_t size, const char *file)
{
    sprintf_s(out, size, "%s\\users\\profiles\\%s\\%s", g_root, g_profile_id, file);
}

// Has the game write the operation that is on screen and returns the file's contents and name.
static bool save_operation(ObjMan *om, std::vector<unsigned char> &out, char *file, size_t file_size)
{
    static const unsigned char sig[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x7c, 0x24, 0x10, 0x55, 0x48, 0x8d, 0xac, 0x24, 0x10, 0xff, 0xff, 0xff };
    void *op = operation_on_screen(om);
    HMODULE m = GetModuleHandleA("adv_opgame.codelib");
    if (!op || !m) return false;
    if (memcmp((char *)m + 0xd6660, sig, sizeof sig)) { mp_log("map: unknown game build, the operation cannot be saved from here"); return false; }
    FILETIME before;
    GetSystemTimeAsFileTime(&before);
    ((void (*)(void *))((char *)m + 0xd6660))(op);
    // the save it has just written: the newest operation save of the profile
    char find[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAA fd, best = {};
    profile_path(find, sizeof find, "*.opsave");
    HANDLE h = FindFirstFileA(find, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        if (CompareFileTime(&fd.ftLastWriteTime, &best.ftLastWriteTime) > 0) best = fd;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    ULARGE_INTEGER a, b;
    a.LowPart = best.ftLastWriteTime.dwLowDateTime; a.HighPart = best.ftLastWriteTime.dwHighDateTime;
    b.LowPart = before.dwLowDateTime; b.HighPart = before.dwHighDateTime;
    if (!best.cFileName[0] || a.QuadPart + 20000000ull < b.QuadPart) { mp_log("map: the game did not write the operation"); return false; }
    profile_path(path, sizeof path, best.cFileName);
    if (!read_file(path, out) || !opsave::safe_file_name(best.cFileName)) return false;
    strcpy_s(file, file_size, best.cFileName);
    return true;
}

// Host: makes the copy for the joining player. Joining game: saves its map and offers it to the host.
void session_map_give(ObjMan *om)
{
    if (g_map_given || !g_connected) return;
    bool host = g_role == ROLE_HOST && !g_tactical && g_host_mode == HOST_MODE_OPERATION;
    if (!host && !g_map_mode) return;
    std::vector<unsigned char> save;
    char file[96];
    if (!save_operation(om, save, file, sizeof file)) { strcpy_s(g_map_note, "the operation could not be saved"); return; }
    unsigned changed = 0;
    if (host) {
        if (!opsave::mirror(save, &g_map_sidecar, &changed)) {
            mp_log("map: %s has no formations to hand over", file);
            strcpy_s(g_map_note, "this operation cannot be handed over");
            return;
        }
        if (opsave::turn_phase(save) != 0) {               // the turn is past the orders: nothing for him to move
            mp_log("map: not shared, the turn is past its orders phase");
            return;
        }
        g_map_turns = opsave::turns(save);
        g_map_in.clear();
        g_map_in_hash = 0;
        g_map_fetched = false;
    }
    strcpy_s(g_map_file, file);
    g_map_out.swap(save);
    g_map_out_hash = fnv(g_map_out.data(), g_map_out.size());
    g_map_given = true;
    g_map_note[0] = 0;
    if (host) mp_log("map: shared with %s: %s, %zu bytes, %u formations and battle groups turned round", g_peer_name[0] ? g_peer_name : "the other player", g_map_file, g_map_out.size(), changed);
    else mp_log("map: sending this player's moves to the host: %s, %zu bytes", g_map_file, g_map_out.size());
}

// Host: change of mind before the other player has sent anything back.
void session_map_take_back(void)
{
    if (g_role != ROLE_HOST || !g_map_given || g_map_restart_at) return;
    g_map_given = false;
    g_map_out.clear();
    g_map_in.clear();
    mp_log("map: taken back before the other player returned it");
}

static void start_program(const char *arguments)
{
    char cmd[1024];
    if (arguments && *arguments) sprintf_s(cmd, "\"%s\\starter_x64.exe\" %s", g_root, arguments);
    else sprintf_s(cmd, "\"%s\\starter_x64.exe\"", g_root);
    STARTUPINFOA si = { sizeof si };
    PROCESS_INFORMATION pi;
    if (CreateProcessA(0, cmd, 0, 0, FALSE, 0, 0, g_root, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    } else {
        mp_log("map: could not start '%s' (%lu)", cmd, GetLastError());
    }
}

// Host: the other player's map has arrived. The host's own map is saved, his battle groups are copied in
// from his save, and the operation program is started again on the result, where the turn is ended. This
// program ends at once, without its usual shutdown, so it cannot write its older state over the new save.
static void end_turn_here(ObjMan *om);

static void host_merge_and_restart(ObjMan *om)
{
    const char *peer = g_peer_name[0] ? g_peer_name : "the other player";
    std::vector<unsigned char> mine;
    char file[96], path[MAX_PATH], keep[MAX_PATH], args[400];
    g_map_collect = false;
    g_map_given = false;
    if (!save_operation(om, mine, file, sizeof file)) {
        mp_log("map: the host's own map could not be saved; ending the turn without %s's moves", peer);
        end_turn_here(om);
        return;
    }
    unsigned orders = 0, groups = 0;
    if (opsave::has_node(g_map_in, "user_orders") && !opsave::has_node(mine, "user_orders")) {
        // The host has given no order yet, so his save has no orders table to add to (and none can be made
        // in place). The merge is done the other way round: the other player's save, with its tags turned
        // back, takes whatever the host changed.
        std::vector<unsigned char> result = g_map_in;
        unsigned turned = 0, none = 0;
        if (opsave::unmirror(result, g_map_sidecar, &turned)) {
            opsave::set_operation_name(result, opsave::operation_name(mine));
            groups = opsave::merge_moves(result, g_map_out, mine, &none);
            orders = opsave::rows_of(result, "user_orders");
            mine.swap(result);
        }
    } else {
        groups = opsave::merge_moves(mine, g_map_out, g_map_in, &orders);
    }
    std::string id = opsave::operation_id(file, opsave::operation_name(mine));
    profile_path(path, sizeof path, file);
    sprintf_s(keep, "%s.before_merge", path);
    CopyFileA(path, keep, FALSE);
    FILE *f = 0;
    if (id.empty() || fopen_s(&f, path, "wb") || !f) {
        mp_log("map: cannot write the merged operation; ending the turn without %s's moves", peer);
        end_turn_here(om);
        return;
    }
    // The turn is processed with both sides player-led (see opsave::all_player_led); who commands what
    // is put back afterwards from the sidecar, which is kept in a file across the restarts.
    char side[MAX_PATH];
    sprintf_s(side, "%s\\users\\mp_saves", g_root);
    CreateDirectoryA(side, 0);
    sprintf_s(side, "%s\\users\\mp_saves\\_map_sidecar.txt", g_root);
    unsigned led = 0;
    if (opsave::write_sidecar(side, g_map_sidecar)) led = opsave::all_player_led(mine);
    fwrite(mine.data(), 1, mine.size(), f);
    fclose(f);
    mp_log("map: %s's moves are in (%u orders, %u changed values, %u of his formations and groups player-led for the turn); restarting the operation to end the turn",
           peer, orders, groups, led);
    SetEnvironmentVariableA("MP_ENDTURN", "1");
    char ack[16];
    sprintf_s(ack, "%u", g_map_ack);
    SetEnvironmentVariableA("MP_MAP_ACK", ack);          // the restarted program keeps telling the other game it arrived
    sprintf_s(args, "root\\programs\\opgame.progpack, %s, %s, 0, %u", g_profile_id, id.c_str(), opsave::turns(mine));
    start_program(args);
    ExitProcess(0);
}

// Joining game, only when its map cannot be reloaded: back to the host's lobby, ready.
static void joiner_back_to_lobby(void)
{
    char peer[128] = "";
    GetEnvironmentVariableA("MP_PEER", peer, sizeof peer);
    mp_log("map: returning to the lobby");
    SetEnvironmentVariableA("MP_AUTOJOIN", peer);
    SetEnvironmentVariableA("MP_ROLE", 0);
    SetEnvironmentVariableA("MP_MAP", 0);
    start_program(0);
    ExitProcess(0);
}

// Joining game: the host's game has carried out the turn and offers the map of the new one. It is written
// over this player's copy and the operation program is started again on it - the player stays on the
// operational map, with one loading screen between the turns.
static void joiner_reload_map(void)
{
    Joiner j;
    GetEnvironmentVariableA("MP_PEER", j.address, sizeof j.address);
    j.image = g_map_in;
    j.image_hash = g_map_in_hash;
    strcpy_s(j.map_file, g_map_in_file);
    j.map_turns = g_map_in_turns;
    j.map_epoch = g_map_in_epoch;
    mp_log("map: the new turn has arrived (%zu bytes); reloading the map", g_map_in.size());
    session_stop();
    if (j.launch_map(g_root)) ExitProcess(0);
    mp_log("map: %s", j.error);
    joiner_back_to_lobby();
}

// Joining game: the host has started a battle of this turn. The battle is fetched and entered by the join
// launcher (mp_join.exe, the same thing the lobby does); when the battle is over this player's game goes
// back to the host by itself and gets the map of what follows.
static void joiner_into_battle(void)
{
    char peer[128] = "", cmd[MAX_PATH + 160];
    GetEnvironmentVariableA("MP_PEER", peer, sizeof peer);
    mp_log("map: the host has started a battle; entering it");
    SetEnvironmentVariableA("MP_AUTOJOIN", peer);
    SetEnvironmentVariableA("MP_MAP", 0);
    SetEnvironmentVariableA("MP_MAP_EPOCH", 0);
    session_stop();
    sprintf_s(cmd, "\"%s\\mp_join.exe\" %s", g_root, peer);
    STARTUPINFOA si = { sizeof si };
    PROCESS_INFORMATION pi;
    if (CreateProcessA(0, cmd, 0, 0, FALSE, CREATE_NO_WINDOW, 0, g_root, &si, &pi)) ExitProcess(0);
    mp_log("map: could not start mp_join.exe (%lu)", GetLastError());
    joiner_back_to_lobby();
}

// The action behind the map's "next turn" control (adv_opgame+0x36b40) is detoured in both games. While
// both players are on the map it only marks the player who pressed it as done (pressing again takes that
// back); the turn ends when both are - there is no time limit, each player takes as long as he needs.
// Then the host's game collects the other player's moves and carries the turn out. With nobody else on the
// map the control does what it always does.
typedef void (*NextTurn)(void *, u64, const char *);
typedef void (*TurnWorker)(void *, char, void *, void *);
static NextTurn g_next_turn;
static TurnWorker g_turn_worker;

static bool other_player_on_map(void)
{
    return g_role == ROLE_HOST && !g_tactical && g_map_given && g_peer_on_map && g_connected;
}

// The operation has moved on: whatever copy of the map the other player has is of the past.
static void map_moved_on(void)
{
    char v[16];
    g_map_epoch = (g_map_epoch + 1) & 0xffffff;
    if (!g_map_epoch) g_map_epoch = 1;
    sprintf_s(v, "%u", g_map_epoch);
    SetEnvironmentVariableA("MP_MAP_EPOCH", v);
    g_map_given = false;
    g_turn_done = false;
}

// adv_opgame+0x76810(controller, 0, 0, 0): what "next turn" does once confirmed.
static void turn_worker_hook(void *ctrl, char a, void *b, void *c)
{
    if (g_role == ROLE_HOST && !a && !b && !c) map_moved_on();
    g_turn_worker(ctrl, a, b, c);
}

static void begin_collect(void)
{
    if (g_map_collect) return;
    g_map_collect = true;
    g_map_collect_since = GetTickCount();
    mp_log("map: ending the turn - asking %s for his moves", g_peer_name[0] ? g_peer_name : "the other player");
}

static void press_next_turn(void)
{
    const char *peer = g_peer_name[0] ? g_peer_name : "the other player";
    g_turn_done = !g_turn_done;
    g_last_hello = 0;           // a joining game tells the host at once
    if (!g_turn_done) mp_log("map: this player goes on giving orders");
    else mp_log("map: this player has ended the turn%s%s", g_peer_turn_done ? "" : ", waiting for ", g_peer_turn_done ? "" : peer);
}

static void next_turn_hook(void *ctrl, u64 b, const char *event)
{
    bool release = readable(event, 0x10) && *(const float *)(event + 8) < 0;
    if (g_map_mode) {
        if (release && !g_map_given && !g_map_sent) press_next_turn();
        return;
    }
    if (g_map_collect || g_map_restart_at || g_end_turn_pending || g_turn_fix_at) return;      // the turn is being ended already
    if (other_player_on_map()) {
        if (release) press_next_turn();
        return;
    }
    g_next_turn(ctrl, b, event);
}

static void install_map_detours(void)
{
    static const unsigned char sig[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x30, 0x0f, 0x57, 0xc0, 0x49, 0x8b, 0xd8 };
    static bool done;
    bool host = g_role == ROLE_HOST && !g_tactical && g_host_mode == HOST_MODE_OPERATION;
    if (done || (!g_map_mode && !host)) return;
    HMODULE m = GetModuleHandleA("adv_opgame.codelib");
    if (!m) return;
    done = true;
    static const unsigned char worker_sig[] = { 0x48, 0x8b, 0xc4, 0x44, 0x88, 0x40, 0x18, 0x88, 0x50, 0x10, 0x55, 0x56, 0x57, 0x41, 0x56 };
    if (memcmp((char *)m + 0x36b40, sig, sizeof sig)) { mp_log("map: unknown game build, ending the turn is not coordinated between the players"); return; }
    g_next_turn = (NextTurn)install_detour((char *)m + 0x36b40, sizeof sig, next_turn_hook);
    if (host && !memcmp((char *)m + 0x76810, worker_sig, sizeof worker_sig))
        g_turn_worker = (TurnWorker)install_detour((char *)m + 0x76810, sizeof worker_sig, turn_worker_hook);
    if (g_map_mode) mp_log("map: this player's side of the operational map; the turn ends when both players have pressed next turn");
    else mp_log("map: the turn ends when both players have pressed next turn; the other player's moves are collected then");
}

// What the map's "next turn" control does once confirmed (adv_opgame+0x76810).
static void end_turn_here(ObjMan *om)
{
    static const unsigned char sig[] = { 0x48, 0x8b, 0xc4, 0x44, 0x88, 0x40, 0x18, 0x88, 0x50, 0x10, 0x55, 0x56, 0x57, 0x41, 0x56 };
    HMODULE m = GetModuleHandleA("adv_opgame.codelib");
    void *c = menu_controller_of(om);
    // (with the worker detoured its first bytes are our jump; they were checked when the detour went in)
    if (!m || !c || !operation_on_screen(om) || (!g_turn_worker && memcmp((char *)m + 0x76810, sig, sizeof sig))) {
        mp_log("map: the turn could not be ended from here");
        return;
    }
    mp_log("map: ending the turn");
    if (g_turn_worker) turn_worker_hook(c, 0, 0, 0);
    else { map_moved_on(); ((void (*)(void *, char, void *, void *))((char *)m + 0x76810))(c, 0, 0, 0); }
    g_share_after = GetTickCount() + 12000;
}

// Tests: the host presses "next turn"; the operation on screen is written to its save.
static bool g_end_turn_asked, g_test_save_asked;
void session_test_next_turn(void) { g_end_turn_asked = true; }

// Tests: the host behaves as if the operation had moved on (as after a turn), without a turn being
// carried out: the other player's copy of the map is outdated and he is given a new one.
void session_test_new_map(void)
{
    if (g_role != ROLE_HOST || g_tactical) return;
    map_moved_on();
    g_share_after = 0;
    mp_log("test: the operation is declared to have moved on");
}
void session_test_save(void) { g_test_save_asked = true; }

// Host, every frame on the operational map: share the map when the other player is ready, carry out a
// requested end of turn, give up on a player who does not answer.
static void host_map_frame(ObjMan *om, DWORD now)
{
    if (g_role != ROLE_HOST || g_tactical || g_host_mode != HOST_MODE_OPERATION) return;
    bool on_map = operation_on_screen(om) != 0;
    if (!on_map) { g_map_seen_since = 0; return; }
    if (!g_map_seen_since) g_map_seen_since = now;
    bool settled = now - g_map_seen_since > 4000;

    if (g_end_turn_pending) {                               // restarted on the merged save
        if (settled) { g_end_turn_pending = false; end_turn_here(om); g_turn_fix_at = now + 6000; }
        return;
    }
    if (g_turn_fix_at) {                                    // the turn has been processed: the command tags go back
        if (now < g_turn_fix_at) return;
        g_turn_fix_at = 0;
        char side[MAX_PATH], file[96], path[MAX_PATH], args[400];
        opsave::Sidecar sidecar;
        std::vector<unsigned char> save;
        sprintf_s(side, "%s\\users\\mp_saves\\_map_sidecar.txt", g_root);
        if (!opsave::read_sidecar(side, &sidecar) || !save_operation(om, save, file, sizeof file)) {
            mp_log("map: the command tags could not be put back after the turn");
            return;
        }
        unsigned n = opsave::restore_tags(save, sidecar);
        std::string id = opsave::operation_id(file, opsave::operation_name(save));
        profile_path(path, sizeof path, file);
        FILE *f = 0;
        if (id.empty() || fopen_s(&f, path, "wb") || !f) { mp_log("map: cannot write the operation after the turn"); return; }
        fwrite(save.data(), 1, save.size(), f);
        fclose(f);
        DeleteFileA(side);
        mp_log("map: turn processed with both players' moves; %u formations and groups back with the computer's tags, reloading", n);
        SetEnvironmentVariableA("MP_TURNDONE", "1");
        sprintf_s(args, "root\\programs\\opgame.progpack, %s, %s, 0, %u", g_profile_id, id.c_str(), opsave::turns(save));
        start_program(args);
        ExitProcess(0);
    }
    if (g_test_save_asked) {
        g_test_save_asked = false;
        std::vector<unsigned char> save;
        char file[96] = "";
        bool ok = save_operation(om, save, file, sizeof file);
        mp_log("test: operation %s (%s, %zu bytes, turn phase %d, %u orders)", ok ? "saved" : "not saved", file, save.size(),
               ok ? opsave::turn_phase(save) : -1, ok ? opsave::rows_of(save, "user_orders") : 0);
    }
    if (g_end_turn_asked) {                                 // tests: the host presses "next turn"
        g_end_turn_asked = false;
        if (other_player_on_map()) { if (!g_turn_done) press_next_turn(); }
        else end_turn_here(om);
    }
    // both players have ended the turn: now the other player's moves are fetched. No clock runs on either
    // player; only a game that has gone away stops being waited for - the host then ends the turn himself,
    // with the map's control as usual.
    if (g_turn_done && g_peer_turn_done && other_player_on_map() && !g_map_collect && !g_map_restart_at) begin_collect();
    if (!g_connected && (g_turn_done || g_map_collect) && !g_map_restart_at) {
        mp_log("map: the other player's game is gone; the turn is not ended for him");
        g_turn_done = false;
        g_map_collect = false;
        strcpy_s(g_map_note, "the other player's game is gone - next turn ends the turn without him");
    }
    // A copy for the other player: he is in the lobby and ready, and has none yet. Only while orders are
    // being given - with battles pending (controller +0x1260 == 2) the map is no place for him: he stays
    // in the lobby and is taken into the battles.
    char *c = (char *)menu_controller_of(om);
    bool battles_pending = c && *(int *)(c + 0x1260) == 2;
    if (settled && !battles_pending && !g_map_given && !g_map_collect && !g_map_restart_at && g_connected && g_peer_lobby_ready &&
        (!g_share_after || now > g_share_after)) {
        session_map_give(om);
        if (!g_map_given) g_share_after = now + 15000;      // could not be shared now; look again later
    }
}

// asks the other game for the pieces of the map that are still missing
static void request_map_pieces(DWORD now)
{
    if (g_map_in.empty() || g_map_got == g_map_have.size()) return;
    unsigned sent = 0;
    for (unsigned i = 0; i < g_map_have.size() && sent < 24; i++) {
        if (g_map_have[i] || now - g_map_asked[i] < 300) continue;
        ImageRequest r = { make_head(PKT_IMAGE_REQUEST), i };
        r.h.flags = 1;
        send_to(g_peer, &r, sizeof r);
        g_map_asked[i] = now;
        sent++;
    }
}

void session_start(ObjMan *)
{
    char v[128];
    g_tactical = !strcmp(g_program, "tacgame");
    strcpy_s(g_root, g_out_dir);
    if (char *s = strrchr(g_root, '\\')) *s = 0;
    read_profile_name();
    if (GetEnvironmentVariableA("MP_SWAP", v, sizeof v)) g_swap = atoi(v) != 0;
    if (!g_tactical) SetEnvironmentVariableA("MP_SWAPPED", 0);
    g_map_mode = !g_tactical && GetEnvironmentVariableA("MP_MAP", v, sizeof v) && atoi(v);
    if (!g_tactical && GetEnvironmentVariableA("MP_ENDTURN", v, sizeof v)) {
        g_end_turn_pending = true;
        SetEnvironmentVariableA("MP_ENDTURN", 0);
        if (GetEnvironmentVariableA("MP_MAP_ACK", v, sizeof v)) g_map_ack = strtoul(v, 0, 10);
        SetEnvironmentVariableA("MP_MAP_ACK", 0);
    }
    if (!g_tactical && GetEnvironmentVariableA("MP_TURNDONE", v, sizeof v)) {
        strcpy_s(g_map_note, "turn ended with both players' moves");
        SetEnvironmentVariableA("MP_TURNDONE", 0);
    }
    if (GetEnvironmentVariableA("MP_MAP_EPOCH", v, sizeof v)) g_map_epoch = g_map_base_epoch = strtoul(v, 0, 10);
    if (GetEnvironmentVariableA("MP_PORT", v, sizeof v)) g_port = (unsigned short)atoi(v);
    if (GetEnvironmentVariableA("MP_HOST_MODE", v, sizeof v))
        g_host_mode = !_stricmp(v, "operation") ? HOST_MODE_OPERATION : !_stricmp(v, "mission") ? HOST_MODE_MISSION : HOST_MODE_SKIRMISH;
    if (GetEnvironmentVariableA("MP_PREFER", v, sizeof v)) { char code[5] = { 0 }; strncpy_s(code, v, 4); memcpy(&g_prefer_side, code, 4); }
    if (!GetEnvironmentVariableA("MP_ROLE", v, sizeof v)) return;
    g_role = !_stricmp(v, "host") ? ROLE_HOST : !_stricmp(v, "join") ? ROLE_JOIN : ROLE_NONE;
    if (g_role != ROLE_NONE && g_tactical) load_battle_description();
    if (g_role == ROLE_HOST && g_tactical) {
        host_swap_sides_if_asked();
        snapshot_operation_saves();
    }
    if (g_role == ROLE_HOST) {
        // a battle changes the operation: the map the other player had before it is of the past
        if (!g_map_epoch) g_map_epoch = (GetTickCount() & 0xffff) | 0x10000;
        if (g_tactical) map_moved_on();
        else { sprintf_s(v, "%u", g_map_epoch); SetEnvironmentVariableA("MP_MAP_EPOCH", v); }
    }
    // a joining game is told by its launcher whether its description is the mirrored one
    if (g_role == ROLE_JOIN && g_tactical && GetEnvironmentVariableA("MP_VERSUS", v, sizeof v)) set_versus(atoi(v) != 0);
    if (g_role == ROLE_HOST) {
        if (!g_host_mode) g_host_mode = HOST_MODE_SKIRMISH;
        find_local_ip();
        host_listen();
    } else if (g_role == ROLE_JOIN && (g_tactical || g_map_mode)) {
        char peer[128] = "127.0.0.1";
        GetEnvironmentVariableA("MP_PEER", peer, sizeof peer);
        unsigned short port = g_port;
        if (char *colon = strchr(peer, ':')) { *colon = 0; port = (unsigned short)atoi(colon + 1); }
        g_peer = {};
        g_peer.sin_family = AF_INET;
        g_peer.sin_port = htons(port);
        if (inet_pton(AF_INET, peer, &g_peer.sin_addr) != 1) {
            addrinfo hints = {}, *res = 0;
            hints.ai_family = AF_INET;
            if (getaddrinfo(peer, 0, &hints, &res) == 0 && res) {
                g_peer.sin_addr = ((sockaddr_in *)res->ai_addr)->sin_addr;
                freeaddrinfo(res);
            } else {
                mp_log("net: cannot resolve '%s'", peer);
            }
        }
        if (open_socket(0)) {
            g_have_peer = true;
            mp_log("net: joining %s:%u", peer, port);
        }
    }
    // A joining game only exists as a battle (started by the menu or mp_join.exe); its operational
    // program, reached after the battle ends, goes back to the multiplayer menu.
}

void session_stop(void)
{
    if (g_sock != INVALID_SOCKET) {
        if (g_have_peer) { Head bye = make_head(PKT_BYE); send_to(g_peer, &bye, sizeof bye); }
        closesocket(g_sock);
        g_sock = INVALID_SOCKET;
    }
    g_have_peer = g_connected = g_bind_pending = false;
}

// ---------------------------------------------------------------- owned units: send transforms
static void send_transforms(ObjMan *om, DWORD now)
{
    static Object *timer;
    if (!timer) timer = engine_find(om, "i_ctrltimer", "ctrltimer");
    unsigned char packet[sizeof(StateHead) + k_units_per_packet * sizeof(UnitRec)];
    StateHead *sh = (StateHead *)packet;
    UnitRec *recs = (UnitRec *)(sh + 1);
    int n = 0;
    auto flush = [&]() {
        sh->h = make_head(PKT_STATE, n);
        sh->snapshot = g_snapshot;
        sh->sim_ms = (u32)g_sim_ms;
        // The engine's timer (base_main, iCtrlTimer): +0x108 speed setting, +0x10c the automatically reduced
        // speed, used only while +0x15d (automatic control) is on and its 16-frame history (+0x158) is full.
        float set = timer ? *(float *)((char *)timer + 0x108) : 1, cur = timer ? *(float *)((char *)timer + 0x10c) : 1;
        bool reduced = timer && ((char *)timer)[0x15d] && *(u32 *)((char *)timer + 0x158) >= 16 && cur < set;
        sh->speed = reduced ? cur : set;
        sh->cur_speed = set;
        sh->paused = timer ? ((char *)timer)[0x15c] : 0;
        sh->phase = (unsigned char)g_phase;
        sh->ready = (unsigned char)g_my_ready;
        sh->pad = 0;
        send_to(g_peer, packet, (int)(sizeof(StateHead) + n * sizeof(UnitRec)));
        g_stat_packets++;
        n = 0;
    };
    u32 count, sent = 0;
    Object **all = all_objects(om, &count);
    g_snapshot++;
    for (u32 i = 0; i < count; i++) {
        u64 key;
        UnitRec r = {};
        if (!unit_key(all[i], &key, &r) || !owns_object(all[i])) continue;
        const char *u = (const char *)all[i];
        const float *pos = (const float *)(u + UNIT_POS), *rot = (const float *)(u + UNIT_ROT);
        Sent &s = g_sent[key];
        bool moved = fabsf(pos[0] - s.pos[0]) + fabsf(pos[1] - s.pos[1]) + fabsf(pos[2] - s.pos[2]) > 0.002f ||
                     fabsf(rot[0] - s.rot0[0]) + fabsf(rot[1] - s.rot0[1]) + fabsf(rot[2] - s.rot0[2]) > 0.0005f;
        // unchanged units are repeated every two seconds, spread out so they do not all go at once
        if (!moved && now - s.when < 2000 + (r.id % 16) * 30) continue;
        memcpy(s.pos, pos, 12);
        memcpy(s.rot0, rot, 12);
        s.when = now;
        memcpy(r.pos, pos, 12);
        for (int row = 0; row < 3; row++) memcpy(r.rot + row * 3, rot + row * 4, 12);
        memcpy(r.lin, u + UNIT_LIN, 12);
        memcpy(r.ang, u + UNIT_ANG, 12);
        recs[n++] = r;
        g_stat_sent++;
        sent++;
        if (n == k_units_per_packet) flush();
    }
    // the clock always goes out: the joining game follows the host's speed, pause and phase, and the host
    // waits for a joining game that has fallen behind
    if (n || !sent) flush();
}

// ---------------------------------------------------------------- followed units: steer towards the owner's state
static void rebuild_units(ObjMan *om)
{
    u32 count;
    Object **all = all_objects(om, &count);
    g_units.clear();
    for (u32 i = 0; i < count; i++) {
        u64 key;
        if (unit_key(all[i], &key, 0)) g_units[key] = all[i];
    }
    g_units_count_seen = count;
}

Object *session_unit(u64 key)
{
    auto it = g_units.find(key);
    return it == g_units.end() ? 0 : it->second;
}

void session_sim_step(ObjMan *om)
{
    if (!g_connected || !g_tactical) return;
    // Received states wait until this game has been simulating for a while: a battle that has only just
    // appeared is still setting its units up, and a vehicle that gets a state loaded into it at that point
    // crashes the renderer (seen with large operation battles). MP_APPLY_DELAY = seconds, default 12.
    static DWORD first_step;
    static int delay = -1;
    if (delay < 0) {
        char v[16];
        delay = GetEnvironmentVariableA("MP_APPLY_DELAY", v, sizeof v) ? atoi(v) * 1000 : 12000;
        first_step = GetTickCount();
    }
    double t0 = seconds_now();
    // (a state is only loaded once the other game's numbers can be translated - see xlate.cpp)
    if (sync_state_enabled() && xlate_ready() && GetTickCount() - first_step >= (DWORD)delay) statesync_apply(om);
    g_prof_load += seconds_now() - t0;
    u32 count;
    all_objects(om, &count);
    if (count != g_units_count_seen) rebuild_units(om);
    shots_sim_step(om);
    trench_sim_step(om);

    // Followed units are not predicted and not left to this game's own physics: they are shown where their
    // owner had them, a tenth of a second ago, moving smoothly from one received transform to the next
    // ("snapshot interpolation"). Transforms arrive about 15 times a second; applying each one as it came -
    // with this game's physics pushing the unit about in between - is what made the other player's
    // vehicles judder. Prediction is for a unit whose owner is far away; here every unit is simulated by
    // its owner's game without any delay, so nothing has to be guessed ahead.
    //   between two transforms: position and orientation are blended;
    //   past the newest (the next one is late): the unit runs on with its last velocity for at most a
    //     quarter of a second, then stands - which also keeps a unit its owner has parked from driving off
    //     under this game's own simulation and being pulled back.
    double now_s = seconds_now();
    for (const UnitRec &r : g_pending) {
        u64 key = ((u64)r.kind << 32) | r.id;
        auto it = g_units.find(key);
        if (it == g_units.end() || owns_object(it->second)) continue;
        Held &h = g_held[key];
        if (h.have && now_s - h.b.at > 0.004) { h.a = h.b; h.have = 2; }       // (same step: just the newer one)
        h.b = { r, now_s };
        if (!h.have) h.have = 1;
    }
    g_pending.clear();

    const double k_delay = 0.10, k_run_on = 0.25;
    for (auto &kv : g_held) {
        Held &h = kv.second;
        auto it = g_units.find(kv.first);
        if (it == g_units.end() || owns_object(it->second)) continue;
        double t = now_s - k_delay;
        float pos[3], rot[9];
        const UnitRec &B = h.b.rec;
        bool moving = true;
        if (h.have == 2 && t < h.b.at && h.b.at - h.a.at > 0.001 && h.b.at - h.a.at < 0.6) {
            const UnitRec &A = h.a.rec;
            float k = t <= h.a.at ? 0.0f : (float)((t - h.a.at) / (h.b.at - h.a.at));
            for (int i = 0; i < 3; i++) pos[i] = A.pos[i] + (B.pos[i] - A.pos[i]) * k;
            for (int i = 0; i < 9; i++) rot[i] = A.rot[i] + (B.rot[i] - A.rot[i]) * k;
        } else {
            double run = t - h.b.at;
            run = run < 0 ? 0 : run > k_run_on ? k_run_on : run;
            moving = t - h.b.at < k_run_on;
            for (int i = 0; i < 3; i++) pos[i] = B.pos[i] + B.lin[i] * (float)run;
            memcpy(rot, B.rot, sizeof rot);
        }
        // the blended rows made square again: first row, second row at right angles to it, third their cross
        float n0 = sqrtf(rot[0] * rot[0] + rot[1] * rot[1] + rot[2] * rot[2]);
        if (n0 > 1e-4f) {
            for (int i = 0; i < 3; i++) rot[i] /= n0;
            float d = rot[3] * rot[0] + rot[4] * rot[1] + rot[5] * rot[2];
            for (int i = 0; i < 3; i++) rot[3 + i] -= d * rot[i];
            float n1 = sqrtf(rot[3] * rot[3] + rot[4] * rot[4] + rot[5] * rot[5]);
            if (n1 > 1e-4f) {
                for (int i = 0; i < 3; i++) rot[3 + i] /= n1;
                rot[6] = rot[1] * rot[5] - rot[2] * rot[4];
                rot[7] = rot[2] * rot[3] - rot[0] * rot[5];
                rot[8] = rot[0] * rot[4] - rot[1] * rot[3];
            } else memcpy(rot, B.rot, sizeof rot);
        } else memcpy(rot, B.rot, sizeof rot);

        char *u = (char *)it->second;
        float *upos = (float *)(u + UNIT_POS), *urot = (float *)(u + UNIT_ROT), *uinv = (float *)(u + UNIT_INV);
        float e[3] = { pos[0] - upos[0], pos[1] - upos[1], pos[2] - upos[2] };
        float err = sqrtf(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
        g_stat_err_sum += err;
        if (err > g_stat_err_max) g_stat_err_max = err;
        if (err > 3.0f) g_stat_snaps++;
        g_stat_applied++;
        memcpy(upos, pos, 12);
        for (int row = 0; row < 3; row++)
            for (int col = 0; col < 3; col++) {
                urot[row * 4 + col] = rot[row * 3 + col];
                uinv[col * 4 + row] = rot[row * 3 + col];
            }
        if (moving) { memcpy(u + UNIT_LIN, B.lin, 12); memcpy(u + UNIT_ANG, B.ang, 12); }
        else { memset(u + UNIT_LIN, 0, 12); memset(u + UNIT_ANG, 0, 12); }
    }
}

// ---------------------------------------------------------------- detours: Start button, battle clock
// Leaving deployment and leaving the initial orders is the player's decision in the stock game. With another
// player connected it takes both: a press marks this player as ready and the phase steps in both games once
// both are. The interface object (i_intereff, adv_interf) has three ways in, all detoured:
//   +0x3b3b0  the "start game" action: deployment -> initial orders -> battle, then pause on/off
//   +0x3b4f0  the Start button / key: in deployment it opens the "end deployment?" box (or goes straight to
//             its result when confirmations are off), in the initial orders it starts the battle
//   +0x41700  message box results; box 3 answered yes = end deployment
// advance_phase() below calls the original +0x3b3b0 when the time has come.
static bool gate_active(void)
{
    return g_tactical && g_role != ROLE_NONE && g_connected && g_phase < 2;
}

static void mark_ready(void)
{
    if (g_my_ready > g_phase) return;
    g_my_ready = g_phase + 1;
    g_last_hello = 0;       // tell the other game at once
    mp_log("ready: this player has finished %s%s", g_phase == 0 ? "deployment" : "the initial orders",
           g_peer_ready > g_phase ? "" : ", waiting for the other player");
}

void session_test_ready(void) { if (gate_active()) mark_ready(); }

// Tests: end this battle as if the player had confirmed the game's "leave for the operational phase" box
// (message box 6, "it_mb_2oper"). Carried out from the logic pass, not from inside the render pipeline.
static bool g_resign_asked;
void session_test_resign(void) { g_resign_asked = true; }

static void resign_if_asked(ObjMan *om)
{
    if (!g_resign_asked || !g_tactical || !g_box_result) return;
    g_resign_asked = false;
    Object *ui = engine_find(om, "i_intereff", "intereff");
    if (!ui) return;
    mp_log("test: leaving the battle through the game's own exit (box 6)");
    *(u32 *)((char *)ui + 0xde8) = 6;
    g_box_result(ui, 1, 0, 0);
}

static u64 start_action_hook(Object *ui, u64 a, u64 b, u64 c)
{
    if (!gate_active()) return g_start_action(ui, a, b, c);
    mark_ready();
    return 0;
}

static void input_action_hook(Object *ui, u64 b, const char *event)
{
    // the engine acts on the event whose value at +8 is negative (the release)
    if (gate_active() && g_phase == 1 && readable(event, 0x10) && *(const float *)(event + 8) < 0) {
        mark_ready();
        return;
    }
    g_input_action(ui, b, event);
}

static void box_result_hook(Object *ui, int result, u64 c, u64 d)
{
    // +0xde8: which box is open. A yes to "end deployment?" becomes a no for the engine (the box closes,
    // nothing else happens) and a ready mark for us.
    if (gate_active() && g_phase == 0 && result && *(u32 *)((char *)ui + 0xde8) == 3) {
        mark_ready();
        result = 0;
    }
    g_box_result(ui, result, c, d);
}

// The engine's battle timer (base_main, iCtrlTimer) turns the frame time and the speed setting into
// simulation steps ("ctrl_tick"), once per rendered frame. Two things are changed while a multiplayer battle
// runs:
//
// 1. A joining game runs on the host's clock. Its own interface keeps setting speed and pause (every frame),
//    and the engine reads them in several places, so the timer's setters themselves are replaced on the
//    timer object (vtable slots 3 and 5 - too short to detour) by versions that apply the host's values
//    whatever the caller asked for. Only the host changes speed or pauses.
//
// 2. Both games simulate at one fixed rate instead of once per rendered frame. Stock, a game at 100 frames
//    per second gives its units three times as many decisions per second as one at 33, and in a versus
//    game each PC simulates its own player's units - the faster PC's units would be quicker to spot, aim
//    and fire. The step function (base_main+0x4200, detoured) is therefore called with a fixed frame time,
//    as often as the real time that has passed asks for.
//
// 3. The two battle clocks are kept together by catching up, never by dropping or redoing anything (the
//    engine cannot replay a stretch of battle: it is not deterministic, and a whole battle cannot be put
//    back from a snapshot). A joining game that is behind the host simulates faster - up to four times, as
//    many steps per frame as its PC manages - until it is level, and slower when it is ahead. A host that
//    gets ahead of the joining game slows down and, half a second of battle time later, stops, until the
//    other game has caught up: both then advance at the pace of the slower PC and neither misses anything.
//    Only a game that is far away in time (it joined a battle already under way) takes over the host's
//    clock instead of replaying minutes of battle.
static bool follows_host_clock(void)
{
    return g_role == ROLE_JOIN && g_tactical && g_connected && g_have_host_clock;
}

static void set_speed_hook(char *ct, float speed)
{
    g_set_speed(ct, follows_host_clock() ? g_host_speed : speed);
}

static void set_pause_hook(char *ct, unsigned char paused)
{
    g_set_pause(ct, follows_host_clock() ? g_host_paused : paused);
}

static void replace_timer_setters(ObjMan *om)
{
    static bool done;
    if (done || g_role != ROLE_JOIN || !g_tactical) return;
    Object *timer = engine_find(om, "i_ctrltimer", "ctrltimer");
    HMODULE base = GetModuleHandleA("base_main.codelib");
    if (!timer || !base) return;
    done = true;
    void **vt = *(void ***)timer;
    if ((char *)vt != (char *)base + 0x23bc8 || (char *)vt[3] != (char *)base + 0x4680 || (char *)vt[5] != (char *)base + 0x3db0) {
        mp_log("net: unknown game build, speed and pause stay local");
        return;
    }
    // a private copy of the class's function table for this one object (with the type information
    // pointer that sits just before it)
    static void *copy[65];
    memcpy(copy, vt - 1, sizeof copy);
    g_set_speed = (SetSpeed)vt[3];
    g_set_pause = (SetPause)vt[5];
    copy[1 + 3] = set_speed_hook;
    copy[1 + 5] = set_pause_hook;
    *(void ***)timer = copy + 1;
    mp_log("net: speed and pause are the host's");
}

static void timer_step_hook(char *ct, float dt, u32 arg)
{
    bool multi = g_tactical && g_connected && g_role != ROLE_NONE;
    double scale = 1;
    if (follows_host_clock()) {
        ct[0x15d] = 0;                              // no automatic slow-down of its own
        *(float *)(ct + 0x108) = g_host_speed;
        *(float *)(ct + 0x10c) = g_host_speed;
        ct[0x15c] = g_host_paused;
        if (g_phase == 2 && g_host_phase == 2) {
            double speed = g_host_speed > 1 ? g_host_speed : 1;
            double behind = (double)g_host_sim_ms - (double)g_sim_ms;      // battle time, milliseconds
            if (!g_clock_adopted || fabs(behind) > 20000 * speed) {
                // first sight of the host's clock, or minutes apart: take it over
                if (fabs(behind) > 2000) mp_log("clock: taking over the host's battle time (%.1f s, was %.1f s)", g_host_sim_ms / 1000.0, g_sim_ms / 1000.0);
                g_sim_ms = g_host_sim_ms;
                g_clock_adopted = true;
                behind = 0;
            }
            if (fabs(behind) > 40) scale = 1 + behind / (1500 * speed);
            scale = scale < 0.25 ? 0.25 : scale > 4 ? 4 : scale;
        }
    } else if (g_role == ROLE_HOST && multi && g_phase == 2 && g_peer_phase == 2 && g_peer_clock_at && GetTickCount() - g_peer_clock_at < 3000) {
        double speed = *(float *)(ct + 0x108) > 1 ? *(float *)(ct + 0x108) : 1;
        double lead = (double)g_sim_ms - (double)g_peer_sim_ms;            // battle time the other game is behind
        double margin = 400 * speed;        // what the network and the reporting interval account for
        if (lead > margin && lead < 20000 * speed) {
            scale = 1 - (lead - margin) / (500 * speed);
            if (scale < 0) scale = 0;
        }
    }
    DWORD tick = GetTickCount();
    if (scale >= 0.5 || g_role != ROLE_HOST) g_waiting_since = 0;
    else if (!g_waiting_since) g_waiting_since = tick ? tick : 1;
    g_clock_factor = (float)scale;
    static double due;
    if (!multi || g_phase != 2 || ct[0x15c] || g_sim_step <= 0) {
        due = 0;
        g_waiting_since = 0;
        g_timer_step(ct, dt, arg);
        return;
    }
    // A joining game that has to catch up may take a dozen steps in one frame; a host takes five at most
    // and otherwise lets its clock run slow (the joining game follows it anyway).
    // Tests: MP_TEST_SPEED=n runs the battle n times as fast in real time, in steps of the usual size.
    static double hurry = -1;
    if (hurry < 0) {
        char v[16];
        hurry = GetEnvironmentVariableA("MP_TEST_SPEED", v, sizeof v) && atof(v) > 0 ? atof(v) : 1;
    }
    double most = g_sim_step * (g_role == ROLE_JOIN ? 12 : 5) * hurry;
    due += dt * scale * hurry;
    if (due > most) due = most;
    double t0 = seconds_now();
    for (int steps = 0; due >= g_sim_step; steps++) {
        if (steps >= 3 && seconds_now() - t0 > 0.06 * hurry) break;     // keep the picture moving; the rest is taken next frame
        due -= g_sim_step;
        // a hair over the step: the timer cuts speed x time down to whole 5 ms, and 0.02f is just under 20
        g_timer_step(ct, (float)(g_sim_step * 1.0005), arg);
    }
}

static void install_detours(void)
{
    static const unsigned char start_sig[] = { 0x40, 0x53, 0x48, 0x83, 0xec, 0x40, 0x48, 0x8b, 0xd9, 0x80, 0xb9, 0xda, 0x0e, 0x00, 0x00, 0x00 };
    static const unsigned char input_sig[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x50, 0x49, 0x8b, 0xd8, 0x48, 0x8b, 0xf9 };
    static const unsigned char box_sig[] = { 0x40, 0x55, 0x48, 0x83, 0xec, 0x30, 0x8b, 0x81, 0xe8, 0x0d, 0x00, 0x00, 0x48, 0x8b, 0xe9 };
    static const unsigned char timer_sig[] = { 0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x70, 0x10, 0x48, 0x89, 0x78, 0x20 };
    static bool done;
    if (done || !g_tactical || g_role == ROLE_NONE) return;
    HMODULE interf = GetModuleHandleA("adv_interf.codelib"), base = GetModuleHandleA("base_main.codelib");
    if (!interf || !base) return;
    done = true;
    char *start = (char *)interf + 0x3b3b0, *input = (char *)interf + 0x3b4f0, *box = (char *)interf + 0x41700, *timer = (char *)base + 0x4200;
    // all three or none: a half-gated Start button would let one player skip the other
    if (!memcmp(start, start_sig, sizeof start_sig) && !memcmp(input, input_sig, sizeof input_sig) && !memcmp(box, box_sig, sizeof box_sig)) {
        g_input_action = (InputAction)install_detour(input, sizeof input_sig, input_action_hook);
        g_box_result = (BoxResult)install_detour(box, sizeof box_sig, box_result_hook);
        g_start_action = (StartAction)install_detour(start, sizeof start_sig, start_action_hook);
    }
    char v[16];
    if (GetEnvironmentVariableA("MP_SIM_HZ", v, sizeof v)) g_sim_step = atoi(v) > 0 ? 1.0 / atoi(v) : 0;
    if (!memcmp(timer, timer_sig, sizeof timer_sig)) g_timer_step = (TimerStep)install_detour(timer, sizeof timer_sig, timer_step_hook);
    mp_log("net: Start button %s, battle clock %s, simulation at %d steps per second",
           g_start_action ? "shared between the players" : "not hooked (unknown game build)",
           !g_timer_step ? "not hooked (unknown game build)" : g_role == ROLE_JOIN ? "follows the host" : "is this game's",
           g_timer_step && g_sim_step > 0 ? (int)(1 / g_sim_step + 0.5) : 0);
}

// Steps the battle phase when both players are ready, or when a joining game is behind the host's phase
// (it joined late). Called strictly while this game is before the battle, where the action steps the phase.
static void advance_phase(ObjMan *om, DWORD now)
{
    if (!g_start_action || g_phase >= 2 || now - g_last_phase_step < 1500) return;
    bool both = g_my_ready > g_phase && g_peer_ready > g_phase;
    bool behind = g_role == ROLE_JOIN && g_have_host_clock && g_host_phase <= 2 && g_host_phase > g_phase;
    if (!both && !behind) return;
    Object *ui = engine_find(om, "i_intereff", "intereff");
    if (!ui) return;
    g_last_phase_step = now;
    mp_log("phase: leaving %s (%s)", g_phase == 0 ? "deployment" : "initial orders", both ? "both players are ready" : "the host is already further");
    g_start_action(ui, 0, 0, 0);
}

// Tests: MP_TEST_MAXTIME=<seconds> shortens the battle. The engine ends a battle when its time is up; that
// time is the float "max_game_time" of the battle description, kept by i_gamespec at +0x304 (adv_game reads
// it there: lea rax,[rdi+0x304] next to the key "f:max_game_time"). Nothing else about the end changes.
// The limit is copied into a countdown when the battle is built (i_trigman +0x29c, seconds left, counted
// down while the battle runs; at zero the game shows its red end-of-battle clock and then ends the battle),
// so that is what is shortened once the battle is under way.
static void shorten_battle_for_tests(ObjMan *om)
{
    static int state;          // 0 not looked at, 1 limit set, 2 done or not wanted
    static float wanted;
    if (state == 2 || !g_tactical) return;
    if (!state) {
        char v[16];
        if (!GetEnvironmentVariableA("MP_TEST_MAXTIME", v, sizeof v) || atof(v) <= 0) { state = 2; return; }
        Object *gs = engine_find(om, "i_gamespec", "gamespec");
        if (!gs || !readable((char *)gs + 0x304, 4)) return;
        wanted = (float)atof(v);
        float *limit = (float *)((char *)gs + 0x304), was = *limit;
        if (!(was >= 60 && was <= 86400)) { mp_log("test: the battle's time limit is not where it was expected (%g); left alone", was); state = 2; return; }
        *limit = wanted;
        state = 1;
        mp_log("test: the battle's time limit is %g s instead of %g s", wanted, was);
    }
    if (g_phase != 2) return;
    Object *tm = engine_find(om, "i_trigman", "trigman");
    if (!tm || !readable((char *)tm + 0x29c, 4)) return;
    state = 2;
    float *left = (float *)((char *)tm + 0x29c);
    if (*left > wanted && *left <= 86400) { mp_log("test: %g s of battle left instead of %g s", wanted, *left); *left = wanted; }
}

// ---------------------------------------------------------------- joining game: the host is gone
// Without the host there is no battle to follow. A few seconds after the host closed its game or stopped
// answering, the joining game starts the menu program again and closes this battle.
static void leave_battle_for_menu(void)
{
    static bool left;
    if (left) return;
    left = true;
    mp_log("net: leaving the battle, back to the menu");
    SetEnvironmentVariableA("MP_ROLE", 0);
    SetEnvironmentVariableA("MP_PEER", 0);
    SetEnvironmentVariableA("MP_VERSUS", 0);
    SetEnvironmentVariableA("MP_WINDOW", 0);
    char cmd[MAX_PATH + 8];
    sprintf_s(cmd, "\"%s\\starter_x64.exe\"", g_root);
    STARTUPINFOA si = { sizeof si };
    PROCESS_INFORMATION pi;
    if (CreateProcessA(0, cmd, 0, 0, FALSE, 0, 0, g_root, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    if (HWND w = main_window()) PostMessageA(w, WM_CLOSE, 0, 0);
}

// ---------------------------------------------------------------- receive
static void on_packet(const sockaddr_in &from, const unsigned char *data, int size, DWORD now)
{
    const Head *h = (const Head *)data;
    if (size < (int)sizeof(Head) || h->magic != k_magic) return;
    bool from_peer = g_have_peer && from.sin_addr.s_addr == g_peer.sin_addr.s_addr && from.sin_port == g_peer.sin_port;

    if (g_role == ROLE_HOST && h->type == PKT_HELLO) {
        if (size < 24) return;
        const Hello *in = (const Hello *)data;
        if (memcmp(in->proto, k_protocol, sizeof k_protocol)) return;
        bool full = size >= (int)sizeof(Hello) && in->version == k_version;
        unsigned prefer = full ? in->prefer_side : 0;
        // The joining player commands the opponent unless it asked for the side the host plays.
        bool versus = !(prefer && prefer == g_user_side);
        Welcome w = { make_head(PKT_WELCOME) };
        memcpy(w.proto, k_protocol, sizeof k_protocol);
        w.version = k_version;
        w.image_size = g_tactical ? (u32)g_image.size() : 0;
        w.image_hash = g_image_hash;
        w.user_side = g_user_side;
        w.enemy_side = g_enemy_side;
        w.in_battle = g_tactical && !g_image.empty();
        w.host_mode = (unsigned char)g_host_mode;
        w.versus = versus;
        w.swap = g_swap;
        strcpy_s(w.name, g_my_name);
        if (g_map_given && !g_tactical && !g_map_restart_at) {
            w.map_size = (u32)g_map_out.size();
            w.map_hash = g_map_out_hash;
            w.map_turns = g_map_turns;
            strcpy_s(w.map_file, g_map_file);
        } else {
            w.map_hash = g_map_ack;                     // the map this game last took back
        }
        w.map_flags = (g_map_collect ? 1 : 0) | (g_turn_done ? 2 : 0) | (g_map_epoch << 8);
        send_to(from, &w, sizeof w);
        if (!full) return;                              // the caller reads the version from the reply and gives up
        char name[24];
        memcpy(name, in->name, sizeof name);
        name[sizeof name - 1] = 0;
        for (char *c = name; *c; c++) if (!isalnum((unsigned char)*c) && *c != '_' && *c != '-') *c = '_';
        if (!from_peer) {
            char ip[64];
            inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
            mp_log("net: player connected from %s:%u (%s)", ip, ntohs(from.sin_port), name[0] ? name : "no name");
            g_sent.clear();            // a new game needs every unit once
            g_held.clear();
            statesync_reset();
            shots_reset();
            g_peer_ready = 0;
            g_peer_swap_known = false;
        }
        if (name[0]) strcpy_s(g_peer_name, name);
        g_peer_lobby_ready = in->lobby_ready != 0;
        g_peer_on_map = in->phase == 0xfe && in->map_epoch == g_map_epoch;
        g_peer_turn_done = g_peer_on_map && in->turn_done != 0;
        if (!g_tactical) {
            // lobby: the joining player's SWAP SIDES presses arrive as a counter
            if (g_peer_swap_known && in->swap_req != g_peer_swap_req && g_host_mode != HOST_MODE_OPERATION) session_toggle_swap();
            g_peer_swap_req = in->swap_req;
            g_peer_swap_known = true;
            SetEnvironmentVariableA("MP_PEER_NAME", g_peer_name[0] ? g_peer_name : 0);
        }
        // the joining game offers its map (the host asked for it): start fetching it
        if (g_peer_on_map && g_map_collect && !g_map_restart_at && in->map_size && in->map_size <= (64u << 20) &&
            in->map_hash != g_map_in_hash) {
            g_map_in.assign(in->map_size, 0);
            g_map_in_hash = in->map_hash;
            g_map_have.assign((in->map_size + k_chunk - 1) / k_chunk, 0);
            g_map_asked.assign(g_map_have.size(), 0);
            g_map_got = 0;
            mp_log("map: %s is sending his moves (%u bytes)", g_peer_name[0] ? g_peer_name : "the other player", in->map_size);
        }
        if (in->phase < 0xfe && in->ready != g_peer_ready) {
            g_peer_ready = in->ready;
            mp_log("ready: the other player has finished %s", g_peer_ready >= 2 ? "the initial orders" : g_peer_ready ? "deployment" : "nothing yet");
        }
        g_peer = from;
        g_have_peer = g_connected = true;
        g_last_rx = now;
        if (g_tactical) set_versus(versus);
        return;
    }
    if (!from_peer) return;
    g_last_rx = now;

    switch (h->type) {
    case PKT_KEEPALIVE:
        break;
    case PKT_IMAGE_REQUEST:
        if (size >= (int)sizeof(ImageRequest) && (g_role == ROLE_HOST || h->flags == 1)) {
            // flags 0: the battle description (host only); 1: the operational map this game is offering
            const std::vector<unsigned char> &blob = h->flags == 1 ? g_map_out : g_image;
            if (h->flags == 1 && !g_map_given) return;
            if (h->flags == 1 && g_role == ROLE_HOST) g_map_fetched = true;
            u32 index = ((const ImageRequest *)data)->index;
            if ((u64)index * k_chunk >= blob.size()) return;
            ImageData d = { make_head(PKT_IMAGE_DATA), index, 0 };
            d.h.flags = h->flags;
            d.size = (u32)min((size_t)k_chunk, blob.size() - (size_t)index * k_chunk);
            memcpy(d.data, blob.data() + (size_t)index * k_chunk, d.size);
            send_to(from, &d, (int)(sizeof d - k_chunk + d.size));
        }
        break;
    case PKT_IMAGE_DATA:
        if ((g_role == ROLE_HOST || g_map_mode) && h->flags == 1 && !g_map_in.empty() && size >= (int)(sizeof(ImageData) - k_chunk)) {
            const ImageData *d = (const ImageData *)data;
            if (d->index >= g_map_have.size() || g_map_have[d->index] || d->size > k_chunk) return;
            if ((size_t)d->index * k_chunk + d->size > g_map_in.size() || size < (int)(sizeof(ImageData) - k_chunk + d->size)) return;
            memcpy(g_map_in.data() + (size_t)d->index * k_chunk, d->data, d->size);
            g_map_have[d->index] = 1;
            if (++g_map_got == g_map_have.size()) {
                if (fnv(g_map_in.data(), g_map_in.size()) != g_map_in_hash) {
                    mp_log("map: the map arrived damaged, fetching it again");
                    g_map_in_hash = 0;
                    g_map_in.clear();
                } else if (g_map_mode) {
                    g_map_reload_at = now + 200;
                } else {
                    // let the other game hear that it arrived, then merge and restart
                    g_map_ack = g_map_in_hash;
                    g_map_restart_at = now + 2500;
                    mp_log("map: the other player's map received");
                }
            }
        }
        break;
    case PKT_WELCOME:
        if (g_role == ROLE_JOIN && size >= (int)sizeof(Welcome)) {
            const Welcome *w = (const Welcome *)data;
            if (!g_connected) {
                g_connected = true;
                mp_log("net: connected to host (host commands %s)", image::side_name(w->user_side));
            }
            memcpy(g_peer_name, w->name, sizeof g_peer_name);
            g_peer_name[sizeof g_peer_name - 1] = 0;
            if (g_map_mode) {
                u32 epoch = w->map_flags >> 8;
                g_peer_turn_done = (w->map_flags & 2) != 0;
                if ((w->map_flags & 1) && !g_map_given && !g_map_sent) g_collect_asked = true;
                if (g_map_given && !g_map_sent && !w->map_size && w->map_hash == g_map_out_hash) {
                    g_map_sent = true;
                    g_turn_done = false;
                    mp_log("map: the host has this player's moves; waiting for the turn to be carried out");
                }
                if (g_map_sent && w->map_size && epoch == g_map_base_epoch && !(w->map_flags & 1) && !w->in_battle) {
                    // the host is on the map of this same turn again: the turn was not carried out
                    g_map_sent = g_map_given = g_turn_done = false;
                    g_map_out.clear();
                    mp_log("map: the host's game did not carry the turn out; this player goes on giving orders");
                }
                if (w->in_battle) {
                    if (!g_map_battle_at) g_map_battle_at = now + 300;
                } else if (w->map_size && w->map_size <= (64u << 20) && epoch != g_map_base_epoch && w->map_hash != g_map_in_hash &&
                           !(w->map_flags & 1) && opsave::safe_file_name(w->map_file)) {
                    // the operation has moved on: the map of the new turn
                    g_map_in.assign(w->map_size, 0);
                    g_map_in_hash = w->map_hash;
                    g_map_have.assign((w->map_size + k_chunk - 1) / k_chunk, 0);
                    g_map_asked.assign(g_map_have.size(), 0);
                    g_map_got = 0;
                    g_map_in_epoch = epoch;
                    g_map_in_turns = w->map_turns;
                    memcpy(g_map_in_file, w->map_file, sizeof g_map_in_file);
                    g_map_in_file[sizeof g_map_in_file - 1] = 0;
                    mp_log("map: the host offers the map of the next turn (%u bytes)", w->map_size);
                }
            }
            if (w->host_mode && w->host_mode != g_host_mode) {
                g_host_mode = w->host_mode;
                if (g_host_mode == HOST_MODE_OPERATION) {
                    mp_log("net: operation battle - units are followed by position only");
                    char peer[128] = "";
                    if (g_tactical && GetEnvironmentVariableA("MP_PEER", peer, sizeof peer)) SetEnvironmentVariableA("MP_AUTOJOIN", peer);
                }
            }
            set_versus(w->versus != 0);
        }
        break;
    case PKT_STATE:
        if (size >= (int)sizeof(StateHead)) {
            const StateHead *sh = (const StateHead *)data;
            if (size < (int)(sizeof(StateHead) + sh->h.count * sizeof(UnitRec))) return;
            if (g_role == ROLE_JOIN) {
                g_host_speed = sh->speed;
                g_host_cur_speed = sh->cur_speed;
                g_host_paused = sh->paused;
                g_host_phase = sh->phase;
                g_host_sim_ms = sh->sim_ms;
                if (sh->ready != g_peer_ready) {
                    g_peer_ready = sh->ready;
                    mp_log("ready: the other player has finished %s", g_peer_ready >= 2 ? "the initial orders" : g_peer_ready ? "deployment" : "nothing yet");
                }
                g_have_host_clock = true;
            } else {
                g_peer_sim_ms = sh->sim_ms;
                g_peer_phase = sh->phase;
                g_peer_clock_at = now ? now : 1;
            }
            const UnitRec *recs = (const UnitRec *)(sh + 1);
            g_pending.insert(g_pending.end(), recs, recs + sh->h.count);
            g_stat_packets++;
        }
        break;
    case PKT_OBJECT:
    case PKT_OBJECT_ACK:
        statesync_on_packet(data, size, send_peer, g_versus);
        break;
    case PKT_SHOT:
    case PKT_SHOT_ACK:
        shots_on_packet(data, size, send_peer);
        break;
    case PKT_BYE:
        mp_log("net: the other player's game closed");
        g_connected = false;
        if (g_role == ROLE_JOIN && g_tactical && !g_host_lost) g_host_lost = now ? now : 1;
        if (g_role == ROLE_HOST) { g_have_peer = false; g_peer_on_map = g_peer_turn_done = false; }
        break;
    }
}

void session_frame(ObjMan *om)
{
    DWORD now = GetTickCount();
    g_prof_frames++;
    update_ai_commander();
    if (g_tactical && (g_role != ROLE_NONE || GetEnvironmentVariableA("MP_LOG_SHOTS", 0, 0))) shots_install(om);
    if (g_tactical && g_role != ROLE_NONE) trench_install();
    shorten_battle_for_tests(om);
    if (g_role == ROLE_HOST && g_bind_pending && now - g_last_bind_try >= 500) host_listen();
    if (g_sock == INVALID_SOCKET) return;

    unsigned char buf[2048];
    for (int i = 0; i < 2048; i++) {
        sockaddr_in from;
        int flen = sizeof from;
        int n = recvfrom(g_sock, (char *)buf, sizeof buf, 0, (sockaddr *)&from, &flen);
        if (n <= 0) break;
        g_stat_bytes += n;
        on_packet(from, buf, n, now);
        if (g_sock == INVALID_SOCKET) return;
    }

    install_detours();
    install_map_detours();
    resign_if_asked(om);
    host_map_frame(om, now);
    if ((g_role == ROLE_HOST && !g_tactical) || g_map_mode) request_map_pieces(now);
    if (g_map_restart_at && now >= g_map_restart_at) { g_map_restart_at = 0; host_merge_and_restart(om); }
    if (g_map_mode && g_collect_asked && !g_map_given && operation_on_screen(om)) { g_collect_asked = false; session_map_give(om); }
    if (g_map_mode && g_test_save_asked && operation_on_screen(om)) {      // tests: how many orders has this player given?
        g_test_save_asked = false;
        std::vector<unsigned char> save;
        char file[96] = "";
        bool ok = save_operation(om, save, file, sizeof file);
        mp_log("test: operation %s (%s, %zu bytes, turn phase %d, %u orders)", ok ? "saved" : "not saved", file, save.size(),
               ok ? opsave::turn_phase(save) : -1, ok ? opsave::rows_of(save, "user_orders") : 0);
    }
    if (g_map_mode && g_end_turn_asked) {                   // tests: this player presses "next turn"
        g_end_turn_asked = false;
        if (!g_turn_done && !g_map_given && !g_map_sent) press_next_turn();
    }
    if (g_map_mode && g_map_battle_at && now >= g_map_battle_at) joiner_into_battle();
    if (g_map_mode && g_map_reload_at && now >= g_map_reload_at) joiner_reload_map();
    replace_timer_setters(om);
    if (g_connected && g_tactical) advance_phase(om, now);
    if (g_role == ROLE_JOIN) {
        if (now - g_last_hello > (g_connected ? 1000u : 500u)) {
            g_last_hello = now;
            // the hello doubles as keep-alive and carries the side wish, so a restarted host learns it again
            Hello hello = { make_head(PKT_HELLO) };
            memcpy(hello.proto, k_protocol, sizeof k_protocol);
            hello.version = k_version;
            hello.prefer_side = g_prefer_side;
            hello.ready = (unsigned char)g_my_ready;
            hello.phase = g_map_mode ? 0xfe : (unsigned char)g_phase;
            hello.lobby_ready = 1;
            strcpy_s(hello.name, g_my_name);
            if (g_map_mode && g_map_given && !g_map_sent) { hello.map_size = (u32)g_map_out.size(); hello.map_hash = g_map_out_hash; }
            hello.turn_done = g_map_mode && g_turn_done;
            hello.map_epoch = g_map_base_epoch;
            send_to(g_peer, &hello, sizeof hello);
        }
    }
    if (g_connected && g_tactical) {
        if (now - g_last_snapshot >= 66) {
            g_last_snapshot = now;
            send_transforms(om, now);
        }
        double t0 = seconds_now();
        if (sync_state_enabled()) statesync_owner_frame(om, now, owns_object, send_peer);
        g_prof_save += seconds_now() - t0;
        xlate_frame(om);
        shots_frame(send_peer);
    }

    if (g_connected && now - g_last_rx > 8000) {
        mp_log("net: connection lost (nothing received for 8 s)");
        g_connected = false;
        g_peer_ready = 0;
        if (g_role == ROLE_JOIN && g_tactical && !g_host_lost) g_host_lost = now ? now : 1;
        if (g_role == ROLE_HOST) { g_have_peer = false; g_peer_on_map = g_peer_turn_done = false; }
    }
    if (g_host_lost && now - g_host_lost > 4000) leave_battle_for_menu();
    if (now - g_last_report >= 5000) {
        if (g_connected && g_tactical && (g_stat_sent || g_stat_applied)) {
            mp_log("net: 5 s: sent %u and applied %u unit transforms, mean correction %.2f m, largest %.2f m, %u jumps, %.1f KB/s in total",
                   g_stat_sent, g_stat_applied, g_stat_applied ? g_stat_err_sum / g_stat_applied : 0.0, g_stat_err_max, g_stat_snaps, g_stat_bytes / 5120.0);
            statesync_report();
            shots_report();
            trench_report();
            if (g_role == ROLE_JOIN && g_have_host_clock)
                mp_log("clock: battle time here %.1f s, on the host %.1f s (time scaled by %.2f to stay on it)",
                       g_sim_ms / 1000.0, g_host_sim_ms / 1000.0, g_clock_factor);
            else if (g_role == ROLE_HOST)
                mp_log("clock: battle time %.1f s, in the other game %.1f s (time scaled by %.2f to wait for it)",
                       g_sim_ms / 1000.0, g_peer_sim_ms / 1000.0, g_clock_factor);
            mp_log("load: 5 s: %u frames, %u simulation steps covering %u ms, %.0f ms saving state, %.0f ms loading state, window %s",
                   g_prof_frames, g_prof_steps, g_prof_step_ms, g_prof_save * 1000, g_prof_load * 1000,
                   GetForegroundWindow() == main_window() ? "in front" : "in the background");
        }
        g_prof_frames = g_prof_steps = g_prof_step_ms = 0;
        g_prof_save = g_prof_load = 0;
        g_last_report = now;
        g_stat_sent = g_stat_applied = g_stat_packets = g_stat_bytes = g_stat_snaps = 0;
        g_stat_err_sum = 0;
        g_stat_err_max = 0;
    }
}
