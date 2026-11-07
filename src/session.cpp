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
// Ownership (mpmod2): the host's game owns every unit - its simulation is the only one that counts, and the
// joining game shows what it sends. The joining player still commands his own forces: his orders travel to
// the host's game (orders.cpp). For owned units a game sends transforms about 15 times a second (this file)
// and the full saved state when it changes (statesync.cpp). MP_HOST_AUTH=0 goes back to mpmod1's way, where
// each player's game owned the units that player commands.
#include "common.h"
#include "build.h"
#include <intrin.h>
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
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <array>

static const int k_units_per_packet = 14;       // 24 + 14 * 80 = 1144 bytes, below any sane MTU

static int g_role;
static int g_host_mode;
static bool g_tactical, g_versus;
static unsigned g_prefer_side;
static SOCKET g_sock = INVALID_SOCKET;
static sockaddr_in g_peer;
static bool g_have_peer, g_connected, g_bind_pending;
// A player reached through Steam (steam.cpp) is kept in the same structure as one reached over UDP: a family
// of its own, the Steam id in the eight spare bytes.
static const unsigned short k_af_steam = 0x5354;
static sockaddr_in steam_addr(u64 id)
{
    sockaddr_in a = {};
    a.sin_family = k_af_steam;
    memcpy(a.sin_zero, &id, 8);
    return a;
}
static bool is_steam(const sockaddr_in &a) { return a.sin_family == k_af_steam; }
static u64 steam_id_of(const sockaddr_in &a) { u64 id; memcpy(&id, a.sin_zero, 8); return id; }
static bool same_addr(const sockaddr_in &a, const sockaddr_in &b)
{
    return a.sin_family == b.sin_family && a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port && !memcmp(a.sin_zero, b.sin_zero, 8);
}
// UPnP: asking the router to pass the hosting port on to this PC. Only when the player has switched it on
// (the multiplayer panel's UPnP box, or MP_UPNP=1 for a game hosted from a command line).
static bool g_upnp_on;
static unsigned short g_upnp_port;         // the port the router has been asked for (0: none yet)
static volatile LONG g_upnp_state;         // 1 asking, 2 the router has opened the port, 3 it has not
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
// While the turn's battles are fought (or the turn is past its orders) the map is shared to look at only: the
// joining player sees the host's newest map, gives no orders and goes into each battle the host starts from
// it; after a battle his game comes back to the map, never to the menu. Host: g_map_view_shared, the copy given
// is such a one (Welcome.map_flags bit 3). Joining game: g_map_view, this copy is; g_map_in_view, the one arriving is.
static bool g_map_view_shared, g_map_view, g_map_in_view;
static int g_battles_seen = -1;         // host: battles pending on the map when last looked at (-1 not yet)
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
static std::unordered_set<u64> g_user_side_units, g_enemy_side_units;   // every unit of this description's player side / other side
static std::unordered_set<u64> g_position_only;     // units whose saved state is not exchanged

// unit transforms
struct Sent { float pos[3], rot0[3]; DWORD when; };
static std::unordered_map<u64, Sent> g_sent;            // what was last sent per owned unit
static std::unordered_map<u64, Object *> g_units;       // local unit per id
static u32 g_units_count_seen;
// The last few transforms received per followed unit, oldest first, with the time each arrived and the
// sender's battle time when it was taken (seconds, below zero outside the battle phase): the unit is shown
// between the two around a moment a little in the past (see session_sim_step). That moment lies more than
// one transform back when one arrives, so two were not enough: the unit was put on the older one and stood.
struct Snap { UnitRec rec; double at, sim; };
enum { HELD_SNAPS = 8 };        // (faster than normal the shown time is up to 1.5 battle s behind: 8 x 0.27 covers it)
struct Held { Snap s[HELD_SNAPS]; int n; float wrote[3], wrote_lin[3]; u64 wrote_ms; int branch; float k; double t, ta, tb, newest; };      // wrote*: (tests) what the last step put there
struct Pending { UnitRec rec; double sim; };
static std::vector<Pending> g_pending;                  // received, applied before the next simulation step
static std::unordered_map<u64, Held> g_held;
// The other game's battle clock as this PC's clock sees it: base at this PC's time `at`, running on at
// `rate` battle seconds a second (followed from the transforms' battle times as they arrive - offset and
// rate both, so a clock that is catching up is not lagged behind); dev: how far the arrivals wander about
// it; interval: how far apart in battle time the transforms are taken.
// measured: battle seconds per real second of the owner's clock, taken over a second or more of transforms
// (win_*: where that window started) - what the rate is kept near, as the speed the owner reports is not always
// what its clock runs at (its automatic slow-down; MP_TEST_SPEED).
// what the owner reported last (spd): when that changes the measured rate is started again.
struct Playout { bool have; double base, at, rate, dev, interval, last, measured, win_at, win_sim, spd; };
static Playout g_play;
static double play_clock(double now) { return g_play.base + g_play.rate * (now - g_play.at); }
// (in battle seconds: at most half a second of real time, whatever the battle's speed)
static double play_delay(void)
{
    return min(max(g_play.interval + 2 * g_play.dev + 0.02, 0.06), 0.5 * max(g_play.rate, 1.0));
}
static float g_host_speed = 1, g_host_cur_speed = 1;
static u32 g_host_sim_ms;
static float g_clock_factor = 1;
static unsigned char g_host_paused;
static bool g_have_host_clock;
static u32 g_peer_sim_ms, g_peer_phase;    // host: the joining game's battle clock and phase, and when they were heard
static DWORD g_peer_clock_at, g_waiting_since;
static bool g_clock_adopted;
static u32 g_snapshot, g_stat_sent, g_stat_applied, g_stat_packets, g_stat_bytes, g_stat_snaps, g_stat_rest;
static double g_stat_err_sum;
static float g_stat_err_max;
static u64 g_sim_ms;
// A soldier (i_human): the vehicle he rides in (0 on foot) and his seat on it (-1 none) - the two fields
// adv_physic's boarding routine writes (beta16 +0x6f9f0: +0x2388 and +0x1da4; 2026 beta +0xbc880: +0x2b90 and +0x2204).
#define SEAT_VEHICLE BF(0x2388, 0x2b90)
#define SEAT_STATION BF(0x1da4, 0x2204)
// The units this game had to move furthest from where its own engine had put them (5 s report): which, how
// often, and - for a soldier - whether he sits on a vehicle here (SEAT_VEHICLE vehicle, SEAT_STATION seat).
struct JumpStat { u32 n; float max; Object *o; float here[3], there[3]; };
static std::unordered_map<u64, JumpStat> g_jumps;
static u64 g_stat_seated;                  // steps a followed soldier was left in his seat here
static void report_jumps(void)
{
    if (g_stat_seated) { mp_log("net: 5 s: %llu times one of the other player's soldiers was left to ride on the vehicle he sits on here", g_stat_seated); g_stat_seated = 0; }
    if (g_jumps.empty()) return;
    std::vector<std::pair<u64, JumpStat>> v(g_jumps.begin(), g_jumps.end());
    std::sort(v.begin(), v.end(), [](const std::pair<u64, JumpStat> &a, const std::pair<u64, JumpStat> &b) { return a.second.n > b.second.n; });
    mp_log("net: 5 s: %u of the other player's units moved more than 3 m by this game; the most often moved:", (unsigned)v.size());
    for (size_t i = 0; i < v.size() && i < 6; i++) {
        const JumpStat &j = v[i].second;
        char *o = (char *)j.o;
        char what[96] = "?";
        if (readable(o, 0x48)) { memcpy(what, o + 8, 63); what[63] = 0; }
        char seat[64] = "";
        if (readable(o, SEAT_VEHICLE + 8) && !strncmp(what, "i_human", 7))
            sprintf_s(seat, ", %s (seat %d)", *(void **)(o + SEAT_VEHICLE) ? "ON A VEHICLE here" : "on foot here", *(int *)(o + SEAT_STATION));
        mp_log("   %s: %u times, up to %.0f m - here %.0f %.0f %.0f, owner %.0f %.0f %.0f%s", what, j.n, j.max,
               j.here[0], j.here[1], j.here[2], j.there[0], j.there[1], j.there[2], seat);
    }
    g_jumps.clear();
}
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
typedef unsigned char (*GetPause)(char *);
static SetSpeed g_set_speed;               // the timer's own accessors, replaced on the timer object of a multiplayer battle
static SetPause g_set_pause;
static GetPause g_get_pause;
// What each player has asked of the battle's clock with his own pause and speed keys.
static float g_my_speed = 1, g_peer_speed = 1;
static bool g_my_pause, g_peer_pause, g_stepping_phase;
static float agreed_speed(void);
// Frames each game draws a second (the other game's comes with its transforms, StateHead.pad2[0]; 0 unknown)
static float g_my_fps, g_peer_fps;
static DWORD g_peer_fps_at;
static float g_ramp = 1;                   // host: the speed the battle runs at while it works up to a faster one
static float running_speed(void);
static const char *slow_game(float *fps, const char *peer);
static double g_sim_step = 0.02;           // multiplayer battles simulate in steps of this many seconds (MP_SIM_HZ)

int session_role(void) { return g_role; }
int session_host_mode(void) { return g_host_mode; }
bool session_connected(void) { return g_connected; }
bool session_versus(void) { return g_versus; }
bool session_versus_or_coop(void) { return g_tactical && g_connected; }
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

static u32 g_steps_seen;
bool session_simulating(void) { return g_steps_seen > 0; }

void session_note_sim_ms(u32 ms)
{
    g_steps_seen++;
    if (g_phase == 2) g_sim_ms += ms;      // battle time
    g_prof_steps += ms != 0;
    g_prof_step_ms += ms;
}

static bool host_decides(void);
void session_note_phase(u32 phase)
{
    static const char *names[] = { "deployment", "initial orders", "battle", "battle over" };
    bool was = host_decides();
    g_phase = phase;
    mp_log("phase: %s", phase < 4 ? names[phase] : "?");
    if (!was && host_decides() && g_versus) mp_log("net: deployment is over - from now on the host's game owns every unit, the other player's as he placed them");
}

// The game's own ESC menu (the column of buttons at the left edge, with the COMBAT panel) is open while the
// battle's interface, i_intereff, has +0xed5 set (adv_interf+0x229c0 sets it and shows the panels; 2026 beta:
// +0x1145, set by adv_interf+0x242a0).
#define IE_MENU_OPEN BF(0xed5, 0x1145)
static Object *g_interface;
bool session_menu_open(void)
{
    return g_tactical && g_interface && readable((char *)g_interface + IE_MENU_OPEN, 1) && *((char *)g_interface + IE_MENU_OPEN) != 0;
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

// skirmish lobby (host): pressed start on the Skirmish screen (session_set_host_ready)
static bool g_host_start_ready;
static bool peer_force_arriving(void);

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
        else if (g_map_view && !g_map_in.empty()) sprintf_s(what, "%s's newest map is arriving", host);
        else if (g_map_view)
            sprintf_s(what, "the map to look at - %s starts the turn's battles and you go into each from here; you give orders again once they are over", host);
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
    else if (g_role == ROLE_JOIN && g_host_lost && !endgame_over())
        sprintf_s(state, "%s has left, returning to the menu", peer);
    else if (!g_connected && g_role == ROLE_HOST && !g_tactical && g_host_mode == HOST_MODE_SKIRMISH && g_host_start_ready)
        sprintf_s(state, "you are ready - waiting for a player at %s:%u; the battle starts when he has joined and is ready", g_local_ip, g_port);
    else if (!g_connected && g_role == ROLE_HOST)
        sprintf_s(state, "waiting for a player at %s:%u", g_local_ip, g_port);
    else if (!g_connected)
        sprintf_s(state, "connecting to the host");
    else if (!g_tactical && (g_map_collect || g_map_restart_at || g_end_turn_pending))
        sprintf_s(state, "ending the turn with %s's moves", peer);
    else if (!g_tactical && g_map_given && g_map_view_shared)
        sprintf_s(state, "%s sees the map while the turn's battles are fought - he goes into each battle you start", peer);
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
    else if (!g_tactical && g_host_mode == HOST_MODE_SKIRMISH && g_host_start_ready)
        sprintf_s(state, "you are ready - the battle starts when %s is ready%s  (start again: not ready; F9: lobby)", peer,
                  g_peer_lobby_ready && peer_force_arriving() ? " (his own force is arriving)" : g_peer_lobby_ready ? "" : " - he is not yet");
    else if (!g_tactical && g_host_mode == HOST_MODE_SKIRMISH)
        sprintf_s(state, "%s has joined and is %s - press start on the Skirmish screen when you are ready  (F9: lobby)", peer, g_peer_lobby_ready ? "ready" : "not ready yet");
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
    else if (g_phase == 2 && endgame_leaving()) {
        int left = (int)endgame_countdown();
        if (left < 0) left = 0;
        if (endgame_leaving() & 1) sprintf_s(state, "your force is withdrawing - the battle ends in %d:%02d", left / 60, left % 60);
        else sprintf_s(state, "%s is withdrawing - the battle ends in %d:%02d", peer, left / 60, left % 60);
    } else if (g_phase == 2 && (endgame_truce() & 3) == 3)
        sprintf_s(state, "ceasefire agreed - the battle ends in %d s", endgame_countdown() > 0 ? (int)endgame_countdown() + 1 : 0);
    else if (g_phase == 2 && (endgame_truce() & 2))
        sprintf_s(state, "%s proposes a ceasefire  (ESC menu: CEASEFIRE to accept)", peer);
    else if (g_phase == 2 && (endgame_truce() & 1))
        sprintf_s(state, "you have proposed a ceasefire - waiting for %s", peer);
    else if (g_phase == 2 && g_connected && g_my_pause && g_peer_pause)
        sprintf_s(state, "paused by both players - it goes on when either of you un-pauses");
    else if (g_phase == 2 && g_connected && g_my_pause)
        sprintf_s(state, "you have asked for a pause - it starts when %s pauses too", peer);
    else if (g_phase == 2 && g_connected && g_peer_pause)
        sprintf_s(state, "%s asks for a pause - pause as well to agree", peer);
    else if (g_phase == 2 && g_connected && g_my_speed != 1 && agreed_speed() != g_my_speed)
        sprintf_s(state, "you have asked for speed x%.1f - the battle runs at x%.1f until %s asks for it too", g_my_speed, agreed_speed(), peer);
    else if (g_phase == 2 && g_connected && g_peer_speed != 1 && agreed_speed() != g_peer_speed)
        sprintf_s(state, "%s asks for speed x%.1f - set your speed to agree", peer, g_peer_speed);
    else if (g_phase == 2 && g_connected && agreed_speed() > 1 && running_speed() < agreed_speed() - 0.01f) {
        float fps;
        const char *slow = slow_game(&fps, peer);
        if (slow) sprintf_s(state, "speed x%.2f of the agreed x%.1f - held while %s draws %.0f frames a second", running_speed(), agreed_speed(), slow, fps);
        else sprintf_s(state, "speed x%.2f, working up to the agreed x%.1f", running_speed(), agreed_speed());
    }
    else if (g_phase == 2 && g_connected && agreed_speed() != 1)
        sprintf_s(state, "speed x%.1f, agreed by both players", agreed_speed());
    else if (g_phase == 2)
        sprintf_s(state, "connected");
    else
        sprintf_s(state, "battle over");
    sprintf_s(g_status, "MULTIPLAYER  -  %s  -  %s", who, state);
    return g_status;
}

static void send_to(const sockaddr_in &to, const void *data, int size)
{
    if (is_steam(to)) steam_send(steam_id_of(to), data, size);
    else sendto(g_sock, (const char *)data, size, 0, (const sockaddr *)&to, sizeof to);
    g_stat_bytes += size;
}

static void send_peer(const void *data, int size)
{
    if (g_sock != INVALID_SOCKET && g_have_peer) send_to(g_peer, data, size);
}

// Full-state replication: a unit's whole saved state comes from its owner's game, so what has happened to
// it - damage, crew, ammunition, where its gun points - is what its owner's game worked out.
// MP_SYNC_STATE=0 switches it off (positions only).
// (It used to be off in operation battles, where it crashed the game: a vehicle's saved state names its
// ammunition crates by their numbers in the saving game's crate store - see state.cpp.)
// mpmod2: the host's game decides everything in a battle (MP_HOST_AUTH=0: each player's game decides for
// the units its player commands, as mpmod1 did).
static bool host_auth(void)
{
    static int on = -1;
    if (on < 0) { char v[8]; on = GetEnvironmentVariableA("MP_HOST_AUTH", v, sizeof v) ? atoi(v) != 0 : 1; }
    return on != 0;
}
bool session_host_auth(void) { return host_auth(); }

// Deployment: each player places his own units, and his game owns them until it ends - placing them is not an
// order (nothing of it goes through the order call), so with the host's game owning them from the start it put
// every unit the joining player placed back where it had it. From the initial orders on the host's game owns
// every unit; it has the joining player's units where he placed them (followed, with their states).
// MP_DEPLOY_HOST=1: the host's game owns everything from the start.
static bool host_decides(void)
{
    if (!host_auth()) return false;
    static int deploy_host = -1;
    if (deploy_host < 0) { char v[8]; deploy_host = GetEnvironmentVariableA("MP_DEPLOY_HOST", v, sizeof v) && atoi(v); }
    return deploy_host || !g_tactical || g_phase != 0;
}

static bool sync_state_enabled(void)
{
    static int forced = -2;
    if (forced == -2) {
        char v[8];
        forced = GetEnvironmentVariableA("MP_SYNC_STATE", v, sizeof v) ? (atoi(v) != 0) : -1;
    }
    return forced != 0;
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

// Which game an object's state comes from. There is exactly one for everything: the joined player's game
// for the units of his side, the host's game for all the rest - the host's own units, and the computer-led
// formations on the host's side that an operation battle has (they are nobody's by their command tags, and
// stood in different places in the two games, hundreds of metres apart, as long as nobody owned them).
static const std::unordered_set<u64> &joined_players_units(void)
{
    return g_role == ROLE_HOST ? g_enemy_side_units : g_user_side_units;
}

static bool owns_object(Object *o)
{
    u64 key;
    if (host_decides() || !g_versus || !unit_key(o, &key, 0)) return g_role == ROLE_HOST;
    const std::unordered_set<u64> &his = joined_players_units();
    if (his.empty()) return g_my_units.count(key) != 0;       // sides not readable: by the command tags
    return (his.count(key) != 0) == (g_role != ROLE_HOST);
}

bool session_owns(Object *o) { return owns_object(o); }
u32 session_phase(void) { return g_phase; }

// A squad is its units: a unit's id is its squad's id plus its place in it (the low five bits).
// Commands: the squads this game's player gives orders to (his selection, his orders).
bool session_commands_squad(u32 id)
{
    if (!g_versus) return g_role == ROLE_HOST;
    static std::unordered_set<u32> squads;
    static const void *built_from;
    static size_t built_size;
    const std::unordered_set<u64> &his = joined_players_units();
    const std::unordered_set<u64> &from = his.empty() ? g_my_units : his;
    if (built_from != &from || built_size != from.size()) {
        squads.clear();
        for (u64 key : from) squads.insert((u32)key & 0x7fffffe0);
        built_from = &from;
        built_size = from.size();
    }
    bool listed = squads.count(id & 0x7fffffe0) != 0;
    return his.empty() ? listed : listed == (g_role != ROLE_HOST);
}

// Ownership of a squad: the game whose simulation decides what it does (its digs, ...).
bool session_owns_squad(u32 id)
{
    if (host_decides()) return g_role == ROLE_HOST;
    return session_commands_squad(id);
}

// Host: a unit the joining player commands - an order of his may name it.
bool session_peer_commands(u64 key)
{
    if (g_role != ROLE_HOST || !g_versus) return false;
    const std::unordered_set<u64> &his = joined_players_units();
    return his.empty() ? g_my_units.count(key) == 0 : his.count(key) != 0;
}
bool session_unit_key(Object *o, u64 *key) { return unit_key(o, key, 0); }

// Something that belongs to a side rather than to a unit - a round of off-map fire, a flare: in a versus
// battle the joined player's game decides for his side, the host's game for every other side; outside
// versus the host's game for all of them.
bool session_owns_side(u32 side)
{
    if (host_auth() || !g_versus || !side) return g_role == ROLE_HOST;
    u32 joined = g_role == ROLE_HOST ? g_enemy_side : g_user_side;
    return (side == joined) == (g_role != ROLE_HOST);
}

// Tests: the middle of the units this game does not own (x, height, z); false when there are none.
bool session_other_units_centre(float *out)
{
    double sum[3] = { 0, 0, 0 };
    int n = 0;
    for (auto &kv : g_units) {
        if (owns_object(kv.second) || !readable((char *)kv.second + UNIT_POS, 12)) continue;
        const float *p = (const float *)((char *)kv.second + UNIT_POS);
        for (int i = 0; i < 3; i++) sum[i] += p[i];
        n++;
    }
    if (!n) return false;
    for (int i = 0; i < 3; i++) out[i] = (float)(sum[i] / n);
    return true;
}

// The squads this game owns (tests).
std::vector<u32> session_own_squads(void)
{
    std::vector<u32> out;
    std::unordered_set<u32> seen;
    for (auto &kv : g_units) {
        u32 id = (u32)kv.first & 0x7fffffe0;
        if (session_commands_squad((u32)kv.first) && seen.insert(id).second) out.push_back(id);
    }
    return out;
}
u64 session_battle_ms(void) { return g_sim_ms; }

// Tests: MP_TEST_DRAWN=1 - in the battle, the five followed units that move fastest are watched where they are
// when each frame is drawn (from the module's place in the render pipeline), for 8 s: per unit the speed, the
// median / largest move from one drawn frame to the next and how many moves were more than three times the
// median ("jumps") or were none while it was moving ("stalls"). What the player sees, not what the steps do.
// The first look from n battle seconds on (MP_TEST_DRAWN=n), then again every 20 s, four times in all.
static u32 g_sim_steps_seen;        // (steps of this game's simulation so far - for the probe below)
void session_drawn_probe(void)
{
    static int on = -1;
    if (on < 0) { char v[8]; on = GetEnvironmentVariableA("MP_TEST_DRAWN", v, sizeof v) && atoi(v); }
    if (!on || !g_tactical || g_phase != 2 || !g_connected) return;
    struct Track { u64 key; Object *o; std::vector<float> d; float last[3]; double t0; };
    static std::vector<Track> tracks;
    static double started, last_pick;
    static u32 steps_was, no_step, one_step, more_steps;
    static bool done;
    double now = seconds_now();
    static int runs;
    static double next_at;
    if (done) {
        if (runs >= 4 || now < next_at) return;
        done = false;
        last_pick = 0;
    }
    if (g_sim_ms < (u64)on * 1000) return;
    if (tracks.empty()) {
        // pick: the followed units that moved most since the last look
        static std::unordered_map<u64, std::array<float, 3>> was;
        if (now - last_pick < 1.0) return;
        last_pick = now;
        std::vector<std::pair<float, u64>> moved;
        for (auto &kv : g_units) {
            if (owns_object(kv.second)) continue;
            const float *p = (const float *)((char *)kv.second + UNIT_POS);
            auto it = was.find(kv.first);
            // (moved more than 2 m in the last second and still moving now)
            const float *lin = (const float *)((char *)kv.second + UNIT_LIN);
            float v = sqrtf(lin[0] * lin[0] + lin[2] * lin[2]);
            if (it != was.end() && v > 1.0f) moved.push_back({ fabsf(p[0] - it->second[0]) + fabsf(p[2] - it->second[2]), kv.first });
            was[kv.first] = { p[0], p[1], p[2] };
        }
        std::sort(moved.begin(), moved.end(), [](const std::pair<float, u64> &a, const std::pair<float, u64> &b) { return a.first > b.first; });
        for (size_t i = 0; i < moved.size() && i < 5 && moved[i].first > 2.0f; i++) {
            Track t = { moved[i].second, g_units[moved[i].second], {}, {}, now };
            memcpy(t.last, (char *)t.o + UNIT_POS, 12);
            tracks.push_back(t);
        }
        started = now;
        steps_was = g_sim_steps_seen;
        return;
    }
    u32 steps = g_sim_steps_seen - steps_was;
    steps_was = g_sim_steps_seen;
    (steps == 0 ? no_step : steps == 1 ? one_step : more_steps)++;
    for (Track &t : tracks) {
        const float *p = (const float *)((char *)t.o + UNIT_POS);
        float d = sqrtf((p[0] - t.last[0]) * (p[0] - t.last[0]) + (p[2] - t.last[2]) * (p[2] - t.last[2]));
        t.d.push_back(d);
        // (the first unit's first three seconds, frame by frame, with what the last step did for it)
        if (&t == &tracks[0] && now - started < 3) {
            auto hi = g_held.find(t.key);
            if (hi != g_held.end()) {
                const Held &h = hi->second;
                mp_log("drawn frame: real %.4f battle %llu steps %u moved %.3f | step: %s k %.2f t %.3f pair %.3f..%.3f newest %.3f, wrote %.3f %.3f vel %.2f %.2f, drawn %.3f %.3f",
                       now, g_sim_ms, steps, d, h.branch == 1 ? "blend" : h.branch == 2 ? "past" : "arr", h.k, h.t, h.ta, h.tb, h.newest,
                       h.wrote[0], h.wrote[2], h.wrote_lin[0], h.wrote_lin[2], p[0], p[2]);
            }
        }
        memcpy(t.last, p, 12);
    }
    if (now - started < 8) return;
    done = true;
    runs++;
    next_at = now + 20;
    mp_log("drawn: look %d at battle time %.0f s - frames with no simulation step before them %u, one %u, more %u", runs, g_sim_ms / 1000.0, no_step, one_step, more_steps);
    no_step = one_step = more_steps = 0;
    for (Track &t : tracks) {
        std::vector<float> s = t.d;
        std::sort(s.begin(), s.end());
        float med = s.empty() ? 0 : s[s.size() / 2], sum = 0;
        int jumps = 0, stalls = 0;
        for (float x : t.d) { sum += x; jumps += med > 0.001f && x > 3 * med + 0.05f; stalls += med > 0.01f && x < 0.0005f; }
        mp_log("drawn: %s - %zu frames in %.1f s, %.2f m/s, move per frame median %.3f m, largest %.3f m, %d jumps, %d stalls",
               t.o->full, t.d.size(), now - started, sum / (now - started), med, s.empty() ? 0 : s.back(), jumps, stalls);
    }
    tracks.clear();
}

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

// ---------------------------------------------------------------- the operation a battle belongs to
// A battle's command line ends "<profiles folder>, <profile>, <campaign>:<operation>, <n>, <turn>" ("*:*, 0, 1"
// for a battle that belongs to no operation). A battle of an operation reads, from its profile folder, the
// operation's files of what earlier battles left on each piece of ground
// (<campaign>_<operation>_<ground>.damsav2: wrecks, ruins, craters, trenches) and builds them into its scene.
// A joined game's battle used to belong to no operation: its scene had less in it than the host's (14
// objects in a Peleshi battle), so everything after them got other render handles - and a unit's saved
// state, full of such handles, could not be loaded (measured: 35,000 of 35,500 states refused).
// The host's battle therefore says which operation it belongs to and hands out those files; the joined
// game puts them into a profile folder of their own and starts its battle as a battle of that operation.
static char g_battle_of[64];
static std::vector<unsigned char> g_extras;
static u32 g_extras_hash;

static void collect_operation_files(void)
{
    std::vector<std::string> items;
    std::string line = GetCommandLineA(), item;
    for (size_t i = 0; i <= line.size(); i++) {
        if (i == line.size() || line[i] == ',') {
            size_t a = item.find_first_not_of(' '), b = item.find_last_not_of(' ');
            items.push_back(a == std::string::npos ? "" : item.substr(a, b - a + 1));
            item.clear();
        } else item += line[i];
    }
    // (2026 beta: a custom mission's battle has ", mis=<mission>" after these - not part of them)
    if (!items.empty() && !items.back().compare(0, 4, "mis=")) items.pop_back();
    size_t last = items.size();
    if (last < 7) return;
    const std::string &folder = items[last - 5], &profile = items[last - 4], &op = items[last - 3];
    size_t colon = op.find(':');
    if (colon == std::string::npos || op == "*:*" || op.size() + items[last - 2].size() + items[last - 1].size() + 5 > sizeof g_battle_of) return;
    // (what goes to the other game, and into a command line there: nothing but names and numbers)
    for (size_t k = last - 3; k < last; k++)
        for (char c : items[k])
            if (!(isalnum((unsigned char)c) || c == '_' || c == ':')) return;
    if (folder.find("..") != std::string::npos || profile.find("..") != std::string::npos) return;
    sprintf_s(g_battle_of, "%s, %s, %s", op.c_str(), items[last - 2].c_str(), items[last - 1].c_str());
    std::string prefix = op;
    prefix[colon] = '_';
    std::string dir = std::string(g_root) + "\\" + folder + "\\" + profile + "\\";
    std::vector<unsigned char> pack(4, 0);
    u32 count = 0;
    WIN32_FIND_DATAA fd;
    // (2026 beta: each piece of ground also has <name>.damsav2.prev, what it was before the last battle - loaded
    //  when a battle is played again or the main file is broken - and that goes along too)
    for (const char *ext : { "_*.damsav2", "_*.damsav2.prev" }) {
        if (strstr(ext, ".prev") && g_bi != BUILD_BETA26) continue;
        HANDLE h = FindFirstFileA((dir + prefix + ext).c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            std::vector<unsigned char> data;
            size_t n = strlen(fd.cFileName);
            if (n > 120 || !read_file((dir + fd.cFileName).c_str(), data) || pack.size() + data.size() > (48u << 20)) continue;
            pack.push_back((unsigned char)n);
            pack.insert(pack.end(), fd.cFileName, fd.cFileName + n);
            u32 size = (u32)data.size();
            pack.insert(pack.end(), (unsigned char *)&size, (unsigned char *)&size + 4);
            pack.insert(pack.end(), data.begin(), data.end());
            count++;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    memcpy(pack.data(), &count, 4);
    g_extras.swap(pack);
    g_extras_hash = fnv(g_extras.data(), g_extras.size());
    mp_log("net: this battle belongs to the operation %s: %u files of what earlier battles left behind (%zu bytes) go to a game that joins",
           g_battle_of, count, g_extras.size());
}

static void load_battle_description(void)
{
    collect_operation_files();
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
    g_user_side_units = image::units_of_side(g_image, g_user_side);
    g_enemy_side_units = image::units_of_side(g_image, g_enemy_side);
    find_position_only_units();
    mp_log("net: battle description %zu bytes (hash %08x), this player commands %s (%zu units)", g_image.size(), g_image_hash,
           image::side_name(g_user_side), g_my_units.size());
    mp_log("net: %zu units on this player's side, %zu on the other side", g_user_side_units.size(), g_enemy_side_units.size());
}

// ---------------------------------------------------------------- versus: no computer commander
// In a versus game both forces have a human commander. The engine's AI commander (i_aicolonel) would keep
// giving orders to the "enemy" force of each game, fighting the other player's orders, so its simulation
// step is switched off in this process: the first instruction of its handler becomes a return.
// A host keeps it off from the start of its battle, so the opponent has no computer-made plan when the other
// player takes command; it comes back if the battle begins without a second player, or with one on the host's side.
//
// An operation battle can have computer-led formations on the HOST's side as well (allies, neighbours).
// Their units are the host's game's, and somebody has to lead them. The commander keeps its formations
// in a list (i_aicolonel +0x398: records of 0x630 bytes, count at +0x3a4; +0 the formation's code, +4 its
// side) and its step leads them one by one (adv_game+0xbbe20 / +0x35be0). In the host's game of a versus
// battle with such formations the commander therefore stays on, and
//   - only the formations of the host's own side are led: the two per-formation routines return at once
//     for any other (those are the other player's);
//   - the routine that looks after the formations of the "enemy" side only (+0x2a670) and the squad
//     housekeeping (+0x37db0, which would also act on the other player's squads) stay out, as they are
//     in every other versus battle.
// 2026 beta: the commander's step (+0x1110e0, called by its ctrl_tick handler +0x129a40) calls the same four
// routines in the same order - housekeeping +0x49490, enemy formations +0x318f0, and per record +0x112190 /
// +0x47220 - over the list at +0x478 (count +0x484; records still 0x630 bytes, +4 the side).
#define AIC_GROUPS BF(0x398, 0x478)
#define AIC_GROUP_COUNT BF(0x3a4, 0x484)
typedef void (*GroupStepA)(char *, char *, float, u64);
typedef void (*GroupStepB)(char *, char *, float);
typedef void (*ColonelStep)(char *, float);
static GroupStepA g_group_a;
static GroupStepB g_group_b;
static ColonelStep g_enemy_groups, g_housekeeping;
static bool g_ai_own_side_only;
static u64 g_stat_led, g_stat_not_led;

static void group_a_hook(char *col, char *group, float dt, u64 x)
{
    if (g_ai_own_side_only && *(u32 *)(group + 4) != g_user_side) { g_stat_not_led++; return; }
    if (g_ai_own_side_only) g_stat_led++;
    g_group_a(col, group, dt, x);
}
static void group_b_hook(char *col, char *group, float dt)
{
    if (g_ai_own_side_only && *(u32 *)(group + 4) != g_user_side) { g_stat_not_led++; return; }
    if (g_ai_own_side_only) g_stat_led++;
    g_group_b(col, group, dt);
}
static void enemy_groups_hook(char *col, float dt) { if (!g_ai_own_side_only) g_enemy_groups(col, dt); }
static void housekeeping_hook(char *col, float dt) { if (!g_ai_own_side_only) g_housekeeping(col, dt); }

// Does the commander of this battle have formations on this game's own side? (-1: not known yet)
static int own_side_formations(ObjMan *om)
{
    Object *col = om ? engine_find(om, "i_aicolonel", "aicolonel") : 0;
    if (!col || !g_user_side) return -1;
    const char *groups = *(const char **)((char *)col + AIC_GROUPS);
    u32 n = *(u32 *)((char *)col + AIC_GROUP_COUNT);
    if (!n) return 0;
    if (n > 64 || !readable(groups, (size_t)n * 0x630)) return -1;
    int mine = 0;
    for (u32 i = 0; i < n; i++) mine += *(const u32 *)(groups + (size_t)i * 0x630 + 4) == g_user_side;
    return mine;
}

static bool install_own_side_filter(HMODULE m)
{
    static int done = -1;
    if (done >= 0) return done != 0;
    static const unsigned char a_old[] = { 0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68, 0x18, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xec };
    static const unsigned char b[] = { 0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68, 0x18, 0x48, 0x89, 0x70, 0x20, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xec };
    static const unsigned char e_old[] = { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c, 0x24, 0x20, 0x56, 0x57, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xec };
    static const unsigned char h[] = { 0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68, 0x10, 0x48, 0x89, 0x70, 0x18, 0x57, 0x41, 0x54 };
    // 2026 beta: two of them begin otherwise (B and the housekeeping are byte for byte as before). The first 18
    // and 16 bytes are moves and pushes (mov rax,rsp / mov [rax+8],rbx / push rbp .. r15; mov [rsp+20h],rbx / push rbp .. r15).
    static const unsigned char a_b26[] = { 0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x08, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0x68, 0xb8 };
    static const unsigned char e_b26[] = { 0x48, 0x89, 0x5c, 0x24, 0x20, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xec, 0x80, 0x00, 0x00, 0x00 };
    (void)m;
    const char *ag = "adv_game.codelib";
    bool b26 = g_bi == BUILD_BETA26;
    char *fa = build_code(ag, BV(0xbbe20, 0xbbe20, 0x112190), b26 ? a_b26 : a_old, b26 ? sizeof a_b26 : sizeof a_old);
    char *fb = build_code(ag, BV(0x35be0, 0x35be0, 0x47220), b, sizeof b);
    char *fe = build_code(ag, BV(0x2a670, 0x2a670, 0x318f0), b26 ? e_b26 : e_old, b26 ? sizeof e_b26 : sizeof e_old);
    char *fh = build_code(ag, BV(0x37db0, 0x37db0, 0x49490), h, sizeof h);
    if (!fa || !fb || !fe || !fh) {
        done = 0;
        return false;
    }
    g_group_a = (GroupStepA)install_detour(fa, BV(19, 19, 18), group_a_hook);
    g_group_b = (GroupStepB)install_detour(fb, 20, group_b_hook);
    g_enemy_groups = (ColonelStep)install_detour(fe, BV(18, 18, 16), enemy_groups_hook);
    g_housekeeping = (ColonelStep)install_detour(fh, 15, housekeeping_hook);
    done = g_group_a && g_group_b && g_enemy_groups && g_housekeeping;
    return done != 0;
}

static void update_ai_commander(ObjMan *om)
{
    // The commander's step, after its first byte (which is what is switched): beta16 +0xbec30 "push rdi; sub rsp,30h;
    // mov rdi,rcx" (40 57 ..); 2026 beta +0x1110e0 "mov [rsp+10h],rbx; mov [rsp+18h],rbp; push rsi .. r15; sub rsp,60h"
    // (48 89 ..) - there the step is the one its ctrl_tick handler +0x129a40 calls, and nothing else leads the formations.
    static const unsigned char sig_old[] = { 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0x8b, 0xf9 };
    static const unsigned char sig_b26[] = { 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x56, 0x57, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xec, 0x60 };
    const unsigned char *sig = g_bi == BUILD_BETA26 ? sig_b26 : sig_old;
    size_t sig_n = g_bi == BUILD_BETA26 ? sizeof sig_b26 : sizeof sig_old;
    unsigned char on = (unsigned char)BV(0x40, 0x40, 0x48);   // the step's own first byte (0xc3, a return, while it is off)
    static int state = -1;          // -1 unknown, 0 active, 1 off, 2 unknown game build
    static int allies = -1;         // host: computer-led formations on its own side (-1 not looked at yet)
    if (!g_tactical || g_role == ROLE_NONE || state == 2) return;
    if (allies < 0 && g_role == ROLE_HOST && g_versus && session_simulating()) {
        allies = own_side_formations(om);
        char sw[8];
        if (allies > 0 && GetEnvironmentVariableA("MP_AI_ALLIES", sw, sizeof sw) && !atoi(sw)) allies = 0;      // MP_AI_ALLIES=0: nobody leads them
        if (allies > 0) {
            HMODULE adv = GetModuleHandleA("adv_game.codelib");
            if (adv && install_own_side_filter(adv)) {
                g_ai_own_side_only = true;
                mp_log("net: %d computer-led formations on this player's side: the AI commander leads them, and only them", allies);
            } else {
                allies = 0;
                mp_log("net: unknown game build, the computer-led formations on this player's side are not led");
            }
        }
    }
    // (tests: MP_TEST_AI_OFF=1 keeps it off in this game whatever the battle - a joining game on the host's
    // side then only follows the computer's units of the host's game, as the other player's units in versus)
    static int test_off = -1;
    if (test_off < 0) { char sw[8]; test_off = GetEnvironmentVariableA("MP_TEST_AI_OFF", sw, sizeof sw) && atoi(sw); }
    bool off = (g_versus && !g_ai_own_side_only) || (g_role == ROLE_HOST && !g_connected && g_phase < 2) || test_off;
    if (!g_versus) g_ai_own_side_only = false;
    if (state == (int)off) return;
    char *m = build_module("adv_game.codelib");
    if (!m) return;
    unsigned char *fn = (unsigned char *)m + BV(0xbec30, 0xbec30, 0x1110e0);
    if (!BV(0xbec30, 0xbec30, 0x1110e0) || !readable(fn, 1 + sig_n) || (*fn != on && *fn != 0xc3) || memcmp(fn + 1, sig, sig_n)) {
        state = 2;
        mp_log("net: unknown game build, the AI commander stays active");
        return;
    }
    DWORD old;
    if (!VirtualProtect(fn, 1, PAGE_EXECUTE_READWRITE, &old)) return;
    *fn = off ? 0xc3 : on;
    VirtualProtect(fn, 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), fn, 1);
    state = off;
    mp_log("net: AI commander switched %s", off ? "off" : "on");
}

static void set_versus(bool versus)
{
    if (versus == g_versus) return;
    g_versus = versus;
    mp_log("net: %s", !versus ? "same side: the host owns every unit" : host_auth() ? "versus: the host's game owns every unit, each player commands his own (his orders go to the host)" :
           "versus: each game owns its own player's units");
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
        g_upnp_state = 3;
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
            g_upnp_state = 2;
        } else {
            mp_log("net: UPnP mapping refused (0x%08lx); relying on a manual port forward of UDP %u", hr, g_port);
            g_upnp_state = 3;
        }
        if (m) m->Release();
        SysFreeString(proto); SysFreeString(client); SysFreeString(desc);
        maps->Release();
    }
    if (nat) nat->Release();
    CoUninitialize();
    return 0;
}

// The player has switched UPnP off again: what this game had the router open is closed.
static DWORD WINAPI upnp_remove_thread(LPVOID arg)
{
    long port = (long)(INT_PTR)arg;
    if (FAILED(CoInitializeEx(0, COINIT_MULTITHREADED))) return 0;
    IUPnPNAT *nat = 0;
    IStaticPortMappingCollection *maps = 0;
    if (SUCCEEDED(CoCreateInstance(__uuidof(UPnPNAT), 0, CLSCTX_ALL, __uuidof(IUPnPNAT), (void **)&nat)) &&
        SUCCEEDED(nat->get_StaticPortMappingCollection(&maps)) && maps) {
        BSTR proto = SysAllocString(L"UDP");
        HRESULT hr = maps->Remove(port, proto);
        mp_log("net: UPnP: the router's forward of UDP %ld %s", port, SUCCEEDED(hr) ? "is removed" : "could not be removed");
        SysFreeString(proto);
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
    // Friends see in Steam that this game can be joined. The player the program before this one was talking
    // to through Steam (the menu before the battle, the battle before the map) may go on talking to this one.
    steam_host(true, g_host_mode == HOST_MODE_OPERATION ? "operation" : g_host_mode == HOST_MODE_MISSION ? "mission" : "skirmish");
    char known[32];
    if (GetEnvironmentVariableA("MP_STEAM_PEER", known, sizeof known) && steam_ready()) {
        u64 id = _strtoui64(known, 0, 10);
        Head hi = make_head(PKT_KEEPALIVE);
        steam_accept(id);
        steam_send(id, &hi, sizeof hi);
    }
    // The operational program hands the port over to the tactical one; for a moment both may exist.
    g_last_bind_try = GetTickCount();
    if (!open_socket(g_port)) {
        if (!g_bind_pending) mp_log("net: UDP port %u is still in use, retrying", g_port);
        g_bind_pending = true;
        return;
    }
    g_bind_pending = false;
    mp_log("net: hosting %son %s UDP %u", g_tactical ? "battle " : "", g_local_ip, g_port);
    if (g_upnp_on && g_upnp_port != g_port) {        // (the router is asked once for each port hosted on)
        g_upnp_port = g_port;
        g_upnp_state = 1;
        CloseHandle(CreateThread(0, 0, upnp_thread, 0, 0, 0));
    }
}

// The port a game hosted from here listens on (the multiplayer menu's port field; 0 = the default, 1943).
// The battle and the operational map are programs of their own, started from this one: they find the port
// in the environment and host on the same one.
void session_set_port(unsigned short port)
{
    g_port = port ? port : k_default_port;
    char v[8];
    sprintf_s(v, "%u", g_port);
    SetEnvironmentVariableA("MP_PORT", g_port == k_default_port ? 0 : v);
}

// The multiplayer panel's UPnP box. The battle and the operational map find the choice in the environment.
void session_set_upnp(bool on)
{
    if (!on && g_upnp_on && g_upnp_port && g_upnp_state == 2)
        CloseHandle(CreateThread(0, 0, upnp_remove_thread, (LPVOID)(INT_PTR)g_upnp_port, 0, 0));
    if (!on) { g_upnp_port = 0; g_upnp_state = 0; }
    g_upnp_on = on;
    SetEnvironmentVariableA("MP_UPNP", on ? "1" : 0);
}

// 0: UPnP is off; 1: the router is being asked; 2: it has opened the port; 3: it has not
int session_upnp(void) { return g_upnp_on ? (g_upnp_state ? (int)g_upnp_state : 1) : 0; }
const char *session_public_ip(void) { return g_external_ip; }

// ---------------------------------------------------------------- lobby: the joining player's own force (skirmish)
// He picks it on his own Skirmish screen (ui.cpp); his game offers the description it made in its hello and
// this game fetches it piece by piece. Kept in users\mp_peer_force.cfgpack, named to the battle program by
// MP_PEER_FORCE (host_swap_sides_if_asked puts it in).
static std::vector<unsigned char> g_force_in;
static std::vector<char> g_force_have;
static std::vector<DWORD> g_force_asked;
static u32 g_force_in_hash, g_force_got, g_peer_force_hash, g_force_refused;
static unsigned g_peer_force_squads, g_peer_force_side, g_peer_force_cost;
// What the host's own force costs on its Skirmish screen (ui.cpp prices it): the most the joining player's may
// cost. Carried to the next menu program after a battle by MP_FORCE_LIMIT.
static unsigned g_force_limit;

void session_set_force_limit(unsigned cost)
{
    if (cost == g_force_limit) return;
    g_force_limit = cost;
    char v[16];
    sprintf_s(v, "%u", cost);
    SetEnvironmentVariableA("MP_FORCE_LIMIT", cost ? v : 0);
    mp_log("lobby: the host's force costs %u - the other player's force may cost as much", cost);
}

unsigned session_force_limit(void)
{
    char v[16];
    if (!g_force_limit && GetEnvironmentVariableA("MP_FORCE_LIMIT", v, sizeof v)) g_force_limit = strtoul(v, 0, 10);
    return g_force_limit;
}

unsigned session_peer_force_cost(void) { return g_peer_force_hash ? g_peer_force_cost : 0; }

// Skirmish lobby (host): a skirmish starts when both players are ready - the host by pressing start on his
// Skirmish screen (ui.cpp holds the start back), the joining player by READY in the lobby, with his own force,
// if he picked one, already here. The battle is then made once, with everything in it (session_prepare_battle):
// a host that started first used to have its battle started a second time when the other player's force came.
void session_set_host_ready(bool on)
{
    if (on == g_host_start_ready) return;
    g_host_start_ready = on;
    g_last_hello = 0;          // (the other game is told at once)
    mp_log("lobby: the host is %s", on ? "ready - the battle starts when the other player is ready too" : "not ready any more");
}

bool session_host_ready(void) { return g_host_start_ready; }

bool session_peer_ready_to_start(void)
{
    return g_role == ROLE_HOST && !g_tactical && g_connected && g_peer_lobby_ready && g_force_in.empty();
}

static bool peer_force_arriving(void) { return !g_force_in.empty(); }

static void force_path(char *out, size_t size) { sprintf_s(out, size, "%s\\users\\mp_peer_force.cfgpack", g_root); }

// The host's battle has started but the joining player is not in it yet (his game is still in the lobby) and it
// is still in deployment: his own force can still go in - the battle is then started again with it
// (late_force_restart). g_force_restarting: that is under way, nobody is to enter this battle any more.
// Off since the skirmish waits for both players (a second loading of the host's battle); MP_LATE_FORCE=1: on.
static bool g_peer_in_lobby, g_force_restarting;
static bool late_force_open(void)
{
    static int allowed = -1;
    if (allowed < 0) { char v[8]; allowed = GetEnvironmentVariableA("MP_LATE_FORCE", v, sizeof v) && atoi(v); }
    return allowed && g_role == ROLE_HOST && g_tactical && g_host_mode == HOST_MODE_SKIRMISH && g_versus && g_phase == 0 && g_peer_in_lobby &&
           !g_image.empty() && !endgame_over();
}
static void late_force_restart(void);

static void peer_force_clear(const char *why)
{
    if (why && (g_peer_force_hash || !g_force_in.empty())) mp_log("lobby: the other player's own force is dropped (%s)", why);
    g_force_in.clear();
    g_force_have.clear();
    g_force_asked.clear();
    g_force_in_hash = g_force_got = g_peer_force_hash = g_force_refused = 0;
    g_peer_force_squads = g_peer_force_side = 0;
    SetEnvironmentVariableA("MP_PEER_FORCE", 0);
    char path[MAX_PATH];
    force_path(path, sizeof path);
    DeleteFileA(path);
}

// The side the host's own battle setup gives the joining player: the enemy side, or the player's side with SWAP
// SIDES. From the host's Skirmish screen while it is (or was) open, else from the screen's last choice in the
// profile's ccombat.cfgpack. 0: not known.
static unsigned g_setup_own, g_setup_enemy;

void session_set_setup_sides(unsigned own, unsigned enemy) { g_setup_own = own; g_setup_enemy = enemy; }

// The setup's year and season, as positions in the Skirmish screen's lists plus 1 (0: not known): from the
// host's screen (kept for the battle program in MP_SETUP_DATE), else the screen's last choice in the profile.
static unsigned g_setup_year, g_setup_period;

void session_set_setup_date(unsigned year, unsigned period)
{
    if (g_setup_year == year + 1 && g_setup_period == period + 1) return;
    g_setup_year = year + 1;
    g_setup_period = period + 1;
    char v[32];
    sprintf_s(v, "%u,%u", g_setup_year, g_setup_period);
    SetEnvironmentVariableA("MP_SETUP_DATE", v);
}

static void profile_setup_path(char *out, size_t size) { sprintf_s(out, size, "%s\\users\\profiles\\%s\\ccombat.cfgpack", g_root, g_profile_id); }

static void setup_date(unsigned *year1, unsigned *period1)
{
    char v[32];
    if (!g_setup_year && GetEnvironmentVariableA("MP_SETUP_DATE", v, sizeof v)) sscanf_s(v, "%u,%u", &g_setup_year, &g_setup_period);
    *year1 = g_setup_year;
    *period1 = g_setup_period;
    if (*year1 || g_tactical) return;
    char path[MAX_PATH];
    profile_setup_path(path, sizeof path);
    std::vector<unsigned char> b;
    if (!read_file(path, b)) return;
    image::Node n = image::find(b, "i_opgame:opgame");
    size_t y = image::field(b, n, "year"), p = image::field(b, n, "period");
    if (y && p) { *year1 = image::u32_at(b.data(), y) + 1; *period1 = image::u32_at(b.data(), p) + 1; }
}

// The side the host commands in its battle setup.
static unsigned planned_host_side(void)
{
    if (g_setup_own && g_setup_enemy) return g_swap ? g_setup_enemy : g_setup_own;
    unsigned joiner = session_planned_joiner_side();
    char path[MAX_PATH];
    profile_setup_path(path, sizeof path);
    std::vector<unsigned char> b;
    if (!joiner || !read_file(path, b)) return 0;
    image::Node n = image::find(b, "i_opgame:opgame");
    size_t u = image::field(b, n, "user_side"), e = image::field(b, n, "enemy_side");
    if (!u || !e) return 0;
    unsigned us = image::u32_at(b.data(), u), es = image::u32_at(b.data(), e);
    return joiner == es ? us : es;
}

// Joining player, CHOOSE MY FORCE: the Skirmish screen reads its setup from the profile when it opens. It is
// set to the host's year and season and to the two sides as they are for this player (0: left as it is).
// The profile's own setup is kept and put back once the screen has read it, and again after its start (the
// screen writes its setup when it starts a battle): the player's own Skirmish setup is not changed by picking
// a force for someone else's battle - and with both games on one PC, sharing the profile, the host's screen
// does not open on the joining player's sides.
static std::vector<unsigned char> g_setup_backup;

void session_restore_skirmish_setup(bool forget)
{
    if (g_setup_backup.empty()) return;
    char path[MAX_PATH];
    profile_setup_path(path, sizeof path);
    FILE *fp = 0;
    if (!fopen_s(&fp, path, "wb") && fp) { fwrite(g_setup_backup.data(), 1, g_setup_backup.size(), fp); fclose(fp); }
    if (forget) g_setup_backup.clear();
}

void session_preset_skirmish(unsigned own, unsigned enemy, unsigned year1, unsigned period1)
{
    char path[MAX_PATH];
    profile_setup_path(path, sizeof path);
    std::vector<unsigned char> b;
    if (!read_file(path, b)) return;
    if (g_setup_backup.empty()) g_setup_backup = b;
    image::Node n = image::find(b, "i_opgame:opgame");
    // (value 0: not known, left alone; year and season are list positions plus 1)
    struct { const char *field; unsigned value, minus; } set[] = { { "user_side", own, 0 }, { "enemy_side", enemy, 0 },
                                                                  { "year", year1, 1 }, { "period", period1, 1 } };
    unsigned changed = 0;
    for (auto &f : set) {
        size_t at = image::field(b, n, f.field);
        if (!at || !f.value || image::u32_at(b.data(), at + 16) != 4) continue;
        image::put_u32(b.data(), at, f.value - f.minus);
        changed++;
    }
    FILE *fp = 0;
    if (!changed || fopen_s(&fp, path, "wb") || !fp) return;
    fwrite(b.data(), 1, b.size(), fp);
    fclose(fp);
    mp_log("ui: CHOOSE MY FORCE - the Skirmish screen is set to the host's year (%u) and season (%u), the %s against the %s",
           year1 ? year1 - 1 : 0, period1 ? period1 - 1 : 0, image::side_name(own), image::side_name(enemy));
}

unsigned session_planned_joiner_side(void)
{
    static DWORD at;
    static unsigned side;
    if (g_setup_own && g_setup_enemy) return g_swap ? g_setup_own : g_setup_enemy;
    if (at && GetTickCount() - at < 2000) return side;
    at = GetTickCount() | 1;
    char path[MAX_PATH];
    sprintf_s(path, "%s\\users\\profiles\\%s\\ccombat.cfgpack", g_root, g_profile_id);
    std::vector<unsigned char> b;
    side = 0;
    if (read_file(path, b)) {
        image::Node n = image::find(b, "i_opgame:opgame");
        size_t u = image::field(b, n, "user_side"), e = image::field(b, n, "enemy_side");
        if (u && e) side = image::u32_at(b.data(), g_swap ? u : e);
    }
    return side;
}

bool session_peer_force(unsigned *squads, unsigned *side)
{
    if (squads) *squads = g_peer_force_squads;
    if (side) *side = g_peer_force_side;
    return g_peer_force_hash != 0;
}

static void force_offer(u32 size, u32 hash, u32 cost)
{
    if (size && hash == g_peer_force_hash) g_peer_force_cost = cost;
    if (!size) { if (!g_tactical && (g_peer_force_hash || !g_force_in.empty())) peer_force_clear("he has none any more"); return; }
    if (hash == g_peer_force_hash || hash == g_force_in_hash || hash == g_force_refused || size > (16u << 20)) return;
    g_force_in.assign(size, 0);
    g_force_in_hash = hash;
    g_force_have.assign((size + k_chunk - 1) / k_chunk, 0);
    g_force_asked.assign(g_force_have.size(), 0);
    g_force_got = 0;
    mp_log("lobby: %s is sending the force he picked (%u bytes)", g_peer_name[0] ? g_peer_name : "the other player", size);
}

static void request_force_pieces(DWORD now)
{
    if (g_force_in.empty() || !g_have_peer) return;
    unsigned sent = 0;
    for (size_t i = 0; i < g_force_have.size() && sent < 24; i++) {
        if (g_force_have[i] || now - g_force_asked[i] < 300) continue;
        ImageRequest r = { make_head(PKT_FORCE_REQUEST), (unsigned)i };
        send_to(g_peer, &r, sizeof r);
        g_force_asked[i] = now ? now : 1;
        sent++;
    }
}

static void force_piece(const ImageData *d, int size)
{
    if (g_force_in.empty() || d->index >= g_force_have.size() || g_force_have[d->index] || d->size > k_chunk) return;
    if ((size_t)d->index * k_chunk + d->size > g_force_in.size() || size < (int)(sizeof(ImageData) - k_chunk + d->size)) return;
    memcpy(g_force_in.data() + (size_t)d->index * k_chunk, d->data, d->size);
    g_force_have[d->index] = 1;
    if (++g_force_got < g_force_have.size()) return;
    std::vector<unsigned char> his;
    his.swap(g_force_in);
    u32 hash = g_force_in_hash;
    g_force_in_hash = 0;
    g_force_have.clear();
    if (fnv(his.data(), his.size()) != hash) { mp_log("lobby: the other player's force arrived damaged, fetching it again"); return; }
    // (in the host's battle: the side he commands in it)
    unsigned side = force::player_side(his), planned = g_tactical ? g_enemy_side : session_planned_joiner_side(), squads = 0, units = 0;
    force::count(his, side, &squads, &units);
    if (!squads || (planned && side != planned)) {
        g_force_refused = hash;
        mp_log("lobby: the other player's force (%u squads of the %s) is not used: he commands the %s", squads, image::side_name(side), image::side_name(planned));
        return;
    }
    char path[MAX_PATH];
    force_path(path, sizeof path);
    FILE *f = 0;
    if (fopen_s(&f, path, "wb") || !f) { mp_log("lobby: cannot keep the other player's force in %s", path); return; }
    fwrite(his.data(), 1, his.size(), f);
    fclose(f);
    SetEnvironmentVariableA("MP_PEER_FORCE", path);
    g_peer_force_hash = hash;
    g_peer_force_squads = squads;
    g_peer_force_side = side;
    mp_log("lobby: the other player's own force arrived: %u squads, %u units of the %s - his side of the battle will be this force", squads, units, image::side_name(side));
    if (g_tactical) late_force_restart();
}

// The host's battle program: the force the battle was made with (MP_PEER_FORCE), so that the joining game is
// told the host has it and does not send it again.
static void note_peer_force(void)
{
    char path[MAX_PATH];
    std::vector<unsigned char> his;
    if (!GetEnvironmentVariableA("MP_PEER_FORCE", path, sizeof path) || !read_file(path, his) || his.empty()) return;
    unsigned units = 0;
    g_peer_force_hash = fnv(his.data(), his.size());
    g_peer_force_side = force::player_side(his);
    force::count(his, g_peer_force_side, &g_peer_force_squads, &units);
}

// Become the host now (multiplayer menu). mode: HOST_MODE_SKIRMISH or HOST_MODE_OPERATION.
void session_host(int mode)
{
    session_stop();
    peer_force_clear(0);
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
    peer_force_clear(0);
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
// Skirmish, the joining player has picked his own force (lobby: CHOOSE MY FORCE): before that, his force takes
// the place of the one the battle setup made for the side he commands (force.h).
// The battle this game plays, made from `setup` (the description the Skirmish screen or the mission wrote): the
// joining player's own force put in, the sides swapped. False when it is `setup` as it is.
static bool make_battle(const std::vector<unsigned char> &setup, std::vector<unsigned char> &turned)
{
    char path[MAX_PATH];
    turned = setup;
    bool changed = false;
    unsigned setup_user = 0, setup_enemy = 0;
    image::sides(setup, &setup_user, &setup_enemy);
    if (g_host_mode == HOST_MODE_SKIRMISH && GetEnvironmentVariableA("MP_PEER_FORCE", path, sizeof path)) {
        std::vector<unsigned char> his;
        unsigned side = g_swap ? setup_user : setup_enemy, his_side = 0;
        char why[200] = "";
        if (!read_file(path, his)) mp_log("lobby: the other player's force (%s) cannot be read - his side is played as set up", path);
        else if ((his_side = force::player_side(his)) != side)
            mp_log("lobby: the other player's force is of the %s and he commands the %s - his side is played as set up", image::side_name(his_side), image::side_name(side));
        else if (force::transplant(turned, his, side, why, sizeof why)) { changed = true; mp_log("lobby: the other player's own force is put into the battle: %s", why); }
        else mp_log("lobby: the other player's force could not be put into the battle (%s) - his side is played as set up", why);
    }
    if (g_swap) {
        if (image::mirror(turned)) changed = true;
        else mp_log("lobby: this battle cannot be swapped, playing it as set up");
    }
    return changed;
}

// Starts this battle program again on `battle`. The setup it was made from is kept (users\mp_battle_setup.cfgpack,
// MP_BATTLE_SETUP) for a force that comes later; MP_SWAPPED tells the new program it is the restarted battle.
// Returns only when that failed, with the description this program plays put back.
static void restart_battle(const std::vector<unsigned char> &battle, const std::vector<unsigned char> &setup, const char *what)
{
    char keep[MAX_PATH], was[8] = "";
    bool restarted = GetEnvironmentVariableA("MP_SWAPPED", was, sizeof was) != 0;
    sprintf_s(keep, "%s\\users\\mp_battle_setup.cfgpack", g_root);
    FILE *f = 0;
    if (fopen_s(&f, keep, "wb") || !f) { mp_log("lobby: cannot keep the battle setup in %s, playing the battle as it is", keep); return; }
    fwrite(setup.data(), 1, setup.size(), f);
    fclose(f);
    if (fopen_s(&f, g_image_file, "wb") || !f) { mp_log("lobby: cannot rewrite the battle description, playing it as it is"); return; }
    fwrite(battle.data(), 1, battle.size(), f);
    fclose(f);
    SetEnvironmentVariableA("MP_BATTLE_SETUP", keep);
    SetEnvironmentVariableA("MP_SWAPPED", "1");
    std::string cmd = GetCommandLineA();
    STARTUPINFOA si = { sizeof si };
    PROCESS_INFORMATION pi;
    if (CreateProcessA(0, &cmd[0], 0, 0, FALSE, 0, 0, g_root, &si, &pi)) {
        mp_log("%s", what);
        ExitProcess(0);
    }
    mp_log("lobby: could not restart the battle (%lu), playing it as it is", GetLastError());
    if (!fopen_s(&f, g_image_file, "wb") && f) { fwrite(g_image.data(), 1, g_image.size(), f); fclose(f); }
    if (!restarted) { SetEnvironmentVariableA("MP_SWAPPED", 0); SetEnvironmentVariableA("MP_BATTLE_SETUP", 0); }
}

// Host, menu program: the Skirmish or Missions screen has written the battle description and the battle program
// is about to be started on it. The joining player's own force and swapped sides go in now, so that the battle
// program starts on the battle as it is played - it used to start, make the change and start itself again (a
// second loading screen). The setup is kept for MP_LATE_FORCE; MP_SWAPPED tells the battle program it is done.
void session_prepare_battle(const char *path)
{
    if (g_role != ROLE_HOST || g_tactical || g_host_mode == HOST_MODE_OPERATION) return;
    SetEnvironmentVariableA("MP_SWAPPED", 0);
    SetEnvironmentVariableA("MP_BATTLE_SETUP", 0);
    std::vector<unsigned char> setup, battle;
    if (!read_file(path, setup)) { mp_log("lobby: cannot read the battle description %s - the battle program makes the battle", path); return; }
    if (!make_battle(setup, battle)) return;
    char keep[MAX_PATH];
    sprintf_s(keep, "%s\\users\\mp_battle_setup.cfgpack", g_root);
    FILE *f = 0;
    if (fopen_s(&f, keep, "wb") || !f) { mp_log("lobby: cannot keep the battle setup in %s - the battle program makes the battle", keep); return; }
    fwrite(setup.data(), 1, setup.size(), f);
    fclose(f);
    if (fopen_s(&f, path, "wb") || !f) { mp_log("lobby: cannot rewrite the battle description - the battle program makes the battle"); return; }
    fwrite(battle.data(), 1, battle.size(), f);
    fclose(f);
    SetEnvironmentVariableA("MP_BATTLE_SETUP", keep);
    SetEnvironmentVariableA("MP_SWAPPED", "1");
    char v[8];
    mp_log("lobby: the battle is made before it starts (%s%s%s)", GetEnvironmentVariableA("MP_PEER_FORCE", v, 0) ? "the other player's own force" : "",
           GetEnvironmentVariableA("MP_PEER_FORCE", v, 0) && g_swap ? ", " : "", g_swap ? "sides swapped" : "");
}

static void host_swap_sides_if_asked(void)
{
    char v[8];
    if (g_host_mode == HOST_MODE_OPERATION || g_image.empty()) return;      // skirmishes and missions
    note_peer_force();
    if (GetEnvironmentVariableA("MP_SWAPPED", v, sizeof v)) return;         // this is the restarted battle
    SetEnvironmentVariableA("MP_BATTLE_SETUP", 0);
    std::vector<unsigned char> turned;
    if (!make_battle(g_image, turned)) return;
    restart_battle(turned, g_image, g_swap && !GetEnvironmentVariableA("MP_PEER_FORCE", v, 0) ? "lobby: sides swapped, restarting the battle from the turned description"
                                                                                           : "lobby: restarting the battle with the other player's own force");
}

// The joining player's own force has arrived while the host's battle was in deployment, before he entered it:
// the battle is made again from its setup, with his force, and started again. His game, in the lobby, then
// takes the new battle.
static void late_force_restart(void)
{
    std::vector<unsigned char> setup, battle;
    char path[MAX_PATH];
    if (!GetEnvironmentVariableA("MP_BATTLE_SETUP", path, sizeof path) || !read_file(path, setup)) setup = g_image;
    if (!make_battle(setup, battle)) { mp_log("lobby: the other player's own force could not be put into this battle - his side is played as it is"); return; }
    g_force_restarting = true;
    restart_battle(battle, setup, "lobby: the other player's own force arrived during deployment - restarting the battle with it");
    g_force_restarting = false;
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

// What the other player has identified of the host's battle groups, by operation (opsave::Known).
static void known_path(char *out, size_t size, const std::string &operation)
{
    sprintf_s(out, size, "%s\\users\\mp_saves", g_root);
    CreateDirectoryA(out, 0);
    sprintf_s(out, size, "%s\\users\\mp_saves\\_known_%s.txt", g_root, operation.c_str());
}

// The host's battle groups the other player's intelligence found out while the host's game carried a turn
// out (ability_pass_hook): their uids, one per line, until a save of the operation is at hand to name them.
static void intel_path(char *out, size_t size)
{
    sprintf_s(out, size, "%s\\users\\mp_saves", g_root);
    CreateDirectoryA(out, 0);
    sprintf_s(out, size, "%s\\users\\mp_saves\\_intel_found.txt", g_root);
}

// Folds what intelligence found into the other player's knowledge of the operation in `save` (written to
// `kpath` when anything is new); the noted uids are then done with.
static unsigned take_intel(const std::vector<unsigned char> &save, const char *kpath, const std::string &opname, opsave::Known *known)
{
    char path[MAX_PATH];
    intel_path(path, sizeof path);
    FILE *f = 0;
    if (fopen_s(&f, path, "r") || !f) return 0;
    std::set<unsigned> uids;
    unsigned v;
    while (fscanf_s(f, "%u", &v) == 1) uids.insert(v);
    fclose(f);
    unsigned added = opsave::learn_uids(save, uids, known);
    if (added) opsave::write_known(kpath, opname, *known);
    DeleteFileA(path);
    mp_log("map: the other player's intelligence found out %zu of the host's battle groups - %u newly identified for him", uids.size(), added);
    return added;
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
    if (g_bi == BUILD_BETA26) n += copy_matching(from, to, "*.damsav2.prev");      // (the 2026 beta's ground files from before the last battle)
    mp_log("lobby: operation saved before the battle: %d files in users\\mp_saves\\%s_vs_%s", n, g_my_name, peer);
    // the host's battle groups fighting in this battle are identified for the other player from now on
    char op[64] = "", path[MAX_PATH], kpath[MAX_PATH];
    size_t k = strcspn(g_battle_of, ",");
    if (k && k < sizeof op) { memcpy(op, g_battle_of, k); op[k] = 0; }
    if (char *colon = strchr(op, ':')) *colon = '_';
    std::vector<unsigned char> save;
    sprintf_s(path, "%s\\%s.opsave", from, op);
    if (!op[0] || !read_file(path, save)) return;
    std::string opname = opsave::operation_name(save);
    known_path(kpath, sizeof kpath, opname);
    opsave::Known known;
    opsave::read_known(kpath, opname, &known);
    take_intel(save, kpath, opname, &known);
    unsigned added = opsave::learn_fought(save, &known);
    if (added) opsave::write_known(kpath, opname, known);
    mp_log("map: %u of the host's battle groups fight %s in this battle - identified for him from now on", added, peer);
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
static bool g_scouted;            // the other player's map: his own scouting has been worked out
// The operational program's controller is i_opgame itself (its constructor puts its own address at +0x100, in
// every build). Its fields from +0x1200 on are 0x10 further on in the 2026 beta, and from +0x1260 on 0x14
// (the mode switch adv_opgame+0xac7a0 writes +0x1210; the scouting pass +0x35540 tests +0x1268 against 0x11;
// the next-turn control +0x370a0 tests +0x1274 against 2 - each where the 2024 builds' routines have +0x1200,
// +0x1258, +0x1260). The operation at +0x1d8 stays.
#define OPC_MODE BF(0x1200, 0x1210)         // 0 on an operation's map, 4 in the menus
#define OPC_SCREEN BF(0x1258, 0x1268)       // the menu screen: 0 main menu, 2 operations, 0x11 skirmish
#define OPC_STAGE BF(0x1260, 0x1274)        // the turn's stage: 2 while its battles are fought
#define OPC_QUICK_DATA BF(0x1360, 0x13e0)   // a quick battle's operation data, inside the controller (screen 0x11)
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
    if (!c || (*(int *)(c + OPC_SCREEN) != 0 && *(int *)(c + OPC_SCREEN) != 2) || *(int *)(c + OPC_MODE) != 0) return 0;
    void *op = *(void **)(c + 0x1d8);
    return readable(op, 0x100) ? op : 0;
}

bool session_on_op_map(ObjMan *om) { return operation_on_screen(om) != 0; }

// The other player's map (his side's own operation on the host's state): what he sees now of the host's
// groups is worked out by the game's own scouting pass, adv_opgame+0x35540(controller), which goes by the
// operation's side - his, in the counterpart operation. The pass is run once his map is up. It keeps what
// the map came with as identified (bit 1) and works "seen now" (bit 0) out from his groups' scouting range.
// Battle groups: 0x6f0-byte records at operation data +200 (count +0xd4); +0 side, +8 command tag, +0x344
// what is known of it. The operation data and the operation's side as the pass finds them.
// (2026 beta: the pass is at the same address and reads the operation the same way - the data, its groups and
// their fields where they were - but for the controller's screen number and quick-battle data, OPC_*.)
static char *operation_data(char *op)
{
    if (!readable(op, 0x100)) return 0;
    if (!op[0xa8]) return *(char **)(op + 0x28) + (size_t)*(u32 *)(op + 0x18) * 0x710;
    char *ctl = *(char **)(op + 0x20);
    return readable(ctl, 0x1400) ? ctl + (*(int *)(ctl + OPC_SCREEN) != 0x11 ? 0x240 : OPC_QUICK_DATA) : 0;
}

static void joiner_scouting(ObjMan *om)
{
    static const unsigned char sig_old[] = { 0x40, 0x53, 0x55, 0x56, 0x41, 0x55, 0x41, 0x56, 0x48, 0x81, 0xec, 0x80, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x91, 0xd8, 0x01, 0x00, 0x00 };
    static const unsigned char sig_b26[] = { 0x40, 0x53, 0x55, 0x56, 0x41, 0x55, 0x41, 0x57, 0x48, 0x81, 0xec, 0x90, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x91, 0xd8, 0x01, 0x00, 0x00 };
    const unsigned char *sig = g_bi == BUILD_BETA26 ? sig_b26 : sig_old;
    char *pass = build_code("adv_opgame.codelib", BV(0x35540, 0x353e0, 0x35540), sig, sizeof sig_old);
    char *c = (char *)menu_controller_of(om), *op = (char *)operation_on_screen(om);
    if (!pass || !c || !op) { mp_log("map: unknown game build, scouting not worked out for this side"); return; }
    char *data = operation_data(op);
    char *spec = readable(data, 0x600) ? *(char **)(data + 0x5f0) : 0;
    char *groups = readable(data, 0x600) ? *(char **)(data + 200) : 0;
    u32 n = readable(data, 0x600) ? *(u32 *)(data + 0xd4) : 0;
    if (!readable(spec, 0x310) || !groups || !n || !readable(groups, (size_t)n * 0x6f0)) { mp_log("map: the operation's groups were not found, scouting not worked out"); return; }
    u32 op_side = *(u32 *)(spec + 0x308), mine = 0;
    for (u32 i = 0; i < n && !mine; i++) if (*(u32 *)(groups + (size_t)i * 0x6f0 + 8) == image::k_user) mine = *(u32 *)(groups + (size_t)i * 0x6f0);
    if (!mine || mine != op_side) { mp_log("map: this map is the host's operation, not this side's: scouting left as the host's copy has it"); return; }
    auto count = [&](unsigned *seen, unsigned *known, unsigned *theirs) {
        *seen = *known = *theirs = 0;
        for (u32 i = 0; i < n; i++) {
            char *g = groups + (size_t)i * 0x6f0;
            if (*(u32 *)g == mine) continue;
            (*theirs)++;
            u32 v = *(u32 *)(g + 0x344);
            *seen += v & 1;
            *known += (v >> 1) & 1;
        }
    };
    unsigned s0, k0, t0, s1, k1, t1;
    count(&s0, &k0, &t0);
    ((void (*)(void *))pass)(c);
    count(&s1, &k1, &t1);
    mp_log("map: this side's own scouting worked out: of the host's %u groups %u seen now (the copy said %u), %u identified", t1, s1, s0, k1);
}
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

// Joining game: the map is only to look at (battles are being fought): it takes no mouse clicks - no orders,
// no battle started here - while the view still moves (mouse, wheel, keys).
bool session_map_view(void) { return g_map_mode && g_map_view && !g_map_battle_at; }

static void profile_path(char *out, size_t size, const char *file)
{
    sprintf_s(out, size, "%s\\users\\profiles\\%s\\%s", g_root, g_profile_id, file);
}

// Has the game write the operation that is on screen and returns the file's contents and name.
static bool save_operation(ObjMan *om, std::vector<unsigned char> &out, char *file, size_t file_size)
{
    static const unsigned char sig_old[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x7c, 0x24, 0x10, 0x55, 0x48, 0x8d, 0xac, 0x24, 0x10, 0xff, 0xff, 0xff };
    // 2026 beta +0xf7ba0: the same routine (three writers, "Error save config", +169) with a smaller frame
    static const unsigned char sig_b26[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x7c, 0x24, 0x10, 0x55, 0x48, 0x8d, 0x6c, 0x24, 0xa9, 0x48, 0x81, 0xec };
    const unsigned char *sig = g_bi == BUILD_BETA26 ? sig_b26 : sig_old;
    void *op = operation_on_screen(om);
    if (!op) return false;
    char *save = build_code("adv_opgame.codelib", BV(0xd6660, 0xd6080, 0xf7ba0), sig, sizeof sig_old);
    if (!save) { mp_log("map: unknown game build, the operation cannot be saved from here"); return false; }
    FILETIME before;
    GetSystemTimeAsFileTime(&before);
    ((void (*)(void *))save)(op);
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

// Host: the turn's battles are being fought - the operational map's controller is in its battles stage
// (+0x1260 == 2, OPC_STAGE).
static bool battles_pending(ObjMan *om)
{
    char *c = (char *)menu_controller_of(om);
    return c && OPC_STAGE && *(int *)(c + OPC_STAGE) == 2;
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
    unsigned changed = 0, identified = 0;
    if (host) {
        // what the other player has identified of the host's groups in this operation so far
        char kpath[MAX_PATH];
        std::string opname = opsave::operation_name(save);
        known_path(kpath, sizeof kpath, opname);
        opsave::Known known;
        opsave::read_known(kpath, opname, &known);
        take_intel(save, kpath, opname, &known);
        if (!opsave::mirror(save, &g_map_sidecar, &changed, &known, &identified)) {
            mp_log("map: %s has no formations to hand over", file);
            strcpy_s(g_map_note, "this operation cannot be handed over");
            return;
        }
        // past the orders (battles being fought): nothing for him to move, the copy is his to look at
        g_map_view_shared = opsave::turn_phase(save) != 0 || battles_pending(om);
        opsave::write_known(kpath, opname, known);
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
    if (host) mp_log("map: shared with %s%s: %s, %zu bytes, %u formations and battle groups turned round; he has identified %u of the host's groups", g_peer_name[0] ? g_peer_name : "the other player",
                     g_map_view_shared ? " to look at (the turn's battles are being fought)" : "", g_map_file, g_map_out.size(), changed, identified);
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
    j.game_build = g_bi;
    j.map_view = g_map_in_view;
    mp_log("map: the host's newest map has arrived (%zu bytes, %s); reloading the map", g_map_in.size(),
           g_map_in_view ? "to look at while the turn's battles are fought" : "to give orders on");
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
    // The way back: when the battle is over this map program is started again, to look at (the host's newest
    // map replaces it as soon as he is back on his) - the player is never left in the menu (create_process_hook).
    if (const char *args = strstr(GetCommandLineA(), "root\\programs\\opgame.progpack")) {
        char epoch[16];
        sprintf_s(epoch, "%u", g_map_base_epoch);
        SetEnvironmentVariableA("MP_MAP_RETURN", args);
        SetEnvironmentVariableA("MP_MAP_RETURN_EPOCH", epoch);
        SetEnvironmentVariableA("MP_MAP_RETURN_PEER", peer);
    }
    // (the launcher is a program of its own without Steam: a host reached through Steam is joined from the
    // menu's lobby, which this game goes back to - ready, so it enters the battle from there by itself)
    if (!_strnicmp(peer, "steam:", 6)) { joiner_back_to_lobby(); return; }
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
    // (a copy to look at does not count: he has no moves to make, the host's next turn is his own)
    return g_role == ROLE_HOST && !g_tactical && g_map_given && !g_map_view_shared && g_peer_on_map && g_connected;
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

// adv_opgame+0x44120(controller): the battle groups' abilities, once a turn, in the host's game - which
// carries every turn out, the other player's groups' abilities with it. Intelligence (ability bit 12) picks
// a group hostile to the group that has it and makes it identified (vis_mode 3) - which the game keeps for
// the operation's side only, the host's. When the group it picks is one of the host's, the other player's
// side has found it out: the pass marks it with turn flag 0x2000 (cleared when the pass next runs). Their
// uids are noted for take_intel. (Battle groups: 0x6f0 bytes at operation data +200, count +0xd4; +0 side,
// +0x340 turn flags, +0x674 uid; the operation's side at *(data +0x5f0) +0x308.)
typedef void (*AbilityPass)(void *);
static AbilityPass g_ability_pass;

static void ability_pass_hook(void *ctrl)
{
    g_ability_pass(ctrl);
    char *data = readable(ctrl, 0x200) ? operation_data(*(char **)((char *)ctrl + 0x1d8)) : 0;
    char *spec = readable(data, 0x600) ? *(char **)(data + 0x5f0) : 0;
    char *groups = readable(data, 0x600) ? *(char **)(data + 200) : 0;
    u32 n = readable(data, 0x600) ? *(u32 *)(data + 0xd4) : 0;
    if (!readable(spec, 0x310) || !groups || !n || n > 4096 || !readable(groups, (size_t)n * 0x6f0)) return;
    u32 host = *(u32 *)(spec + 0x308), found = 0, flagged = 0;
    char path[MAX_PATH];
    intel_path(path, sizeof path);
    FILE *f = 0;
    for (u32 i = 0; i < n; i++) {
        char *g = groups + (size_t)i * 0x6f0;
        if (!(*(u32 *)(g + 0x340) & opsave::k_intel)) continue;
        flagged++;
        if (*(u32 *)g != host) continue;
        if (!f && (fopen_s(&f, path, "a") || !f)) return;
        fprintf(f, "%u\n", *(u32 *)(g + 0x674));
        found++;
    }
    if (f) fclose(f);
    mp_log("map: abilities pass - intelligence found out %u battle groups this turn, %u of them the host's (now known to the other player)", flagged, found);
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
        if (release && g_map_view) mp_log("map: next turn pressed on the map to look at - the host fights the turn's battles first");
        else if (release && !g_map_given && !g_map_sent) press_next_turn();
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
    // the control's handler: in the public build one argument less is passed on (the key flag), so it begins otherwise
    static const unsigned char sig_old[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x30, 0x0f, 0x57, 0xc0, 0x49, 0x8b, 0xd8 };
    static const unsigned char sig_pub[] = { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x0f, 0x57, 0xc0, 0x48, 0x8b, 0xd9, 0x41, 0x0f, 0x2f, 0x40, 0x08 };
    static bool done;
    bool host = g_role == ROLE_HOST && !g_tactical && g_host_mode == HOST_MODE_OPERATION;
    if (done || (!g_map_mode && !host)) return;
    if (!build_module("adv_opgame.codelib")) return;
    done = true;
    static const unsigned char worker_old[] = { 0x48, 0x8b, 0xc4, 0x44, 0x88, 0x40, 0x18, 0x88, 0x50, 0x10, 0x55, 0x56, 0x57, 0x41, 0x56 };
    // 2026 beta: the control +0x370a0 begins as beta16's (the same 16 bytes; it tests the stage at +0x1274 and
    // calls the worker +0x84d00(controller, 0, 0, 0) where beta16's +0x36b40 calls +0x76810); the worker saves rbx
    // too (mov rax,rsp / mov [rax+20h],rbx / mov [rax+18h],r8b / mov [rax+10h],dl / push rbp, rsi, rdi, r12);
    // the abilities pass is +0x468b0 (the turn's routine +0x85d80 calls it where beta16's +0x779a0 calls +0x44120),
    // with intelligence moved into +0x49f30, which marks the group it finds out as before (+0x340 | 0x2000, +0x344 = 3).
    static const unsigned char worker_b26[] = { 0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x20, 0x44, 0x88, 0x40, 0x18, 0x88, 0x50, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54 };
    bool b26 = g_bi == BUILD_BETA26;
    const unsigned char *worker_sig = b26 ? worker_b26 : worker_old;
    size_t worker_n = b26 ? sizeof worker_b26 : sizeof worker_old;
    const unsigned char *sig = BV(sig_old, sig_pub, sig_old);
    size_t sig_n = BV(sizeof sig_old, sizeof sig_pub, sizeof sig_old);
    char *control = build_code("adv_opgame.codelib", BV(0x36b40, 0x369e0, 0x370a0), sig, sig_n);
    if (!control) { mp_log("map: unknown game build, ending the turn is not coordinated between the players"); return; }
    g_next_turn = (NextTurn)install_detour(control, sig_n, next_turn_hook);
    if (char *worker = host ? build_code("adv_opgame.codelib", BV(0x76810, 0x76510, 0x84d00), worker_sig, worker_n) : 0)
        g_turn_worker = (TurnWorker)install_detour(worker, worker_n, turn_worker_hook);
    static const unsigned char pass_old[] = { 0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0x8b, 0xe9 };
    static const unsigned char pass_b26[] = { 0x40, 0x53, 0x55, 0x56, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0x8b, 0xe9, 0x4c, 0x89, 0x64, 0x24, 0x70 };
    const unsigned char *pass_sig = b26 ? pass_b26 : pass_old;
    size_t pass_n = b26 ? sizeof pass_b26 : sizeof pass_old;
    if (char *pass = host ? build_code("adv_opgame.codelib", BV(0x44120, 0x43f50, 0x468b0), pass_sig, pass_n) : 0)
        g_ability_pass = (AbilityPass)install_detour(pass, pass_n, ability_pass_hook);
    else if (host) mp_log("map: unknown game build, what the other player's intelligence finds out is not kept for him");
    if (g_map_mode) mp_log("map: this player's side of the operational map; the turn ends when both players have pressed next turn");
    else mp_log("map: the turn ends when both players have pressed next turn; the other player's moves are collected then");
}

// What the map's "next turn" control does once confirmed (adv_opgame+0x76810).
static void end_turn_here(ObjMan *om)
{
    static const unsigned char sig_old[] = { 0x48, 0x8b, 0xc4, 0x44, 0x88, 0x40, 0x18, 0x88, 0x50, 0x10, 0x55, 0x56, 0x57, 0x41, 0x56 };
    static const unsigned char sig_b26[] = { 0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x20, 0x44, 0x88, 0x40, 0x18, 0x88, 0x50, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54 };
    bool b26 = g_bi == BUILD_BETA26;
    void *c = menu_controller_of(om);
    // (with the worker detoured its first bytes are our jump; they were checked when the detour went in)
    char *worker = g_turn_worker ? 0 : build_code("adv_opgame.codelib", BV(0x76810, 0x76510, 0x84d00), b26 ? sig_b26 : sig_old,
                                                  b26 ? sizeof sig_b26 : sizeof sig_old);
    if (!c || !operation_on_screen(om) || (!g_turn_worker && !worker)) {
        mp_log("map: the turn could not be ended from here");
        return;
    }
    mp_log("map: ending the turn");
    if (g_turn_worker) turn_worker_hook(c, 0, 0, 0);
    else { map_moved_on(); ((void (*)(void *, char, void *, void *))worker)(c, 0, 0, 0); }
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

// Host, once its operation program is up on the map (it is started again after every battle and every
// shared turn): the battle groups of the host that have fought - the game marks them when a battle's result
// comes back to the operation (turn flag 0x10) - are identified for the other player from now on.
static void host_learn_from_battles(ObjMan *om)
{
    std::vector<unsigned char> save;
    char file[96], kpath[MAX_PATH], peer[24] = "player";
    if (g_peer_name[0]) strcpy_s(peer, g_peer_name);
    else GetEnvironmentVariableA("MP_PEER_NAME", peer, sizeof peer);
    if (!save_operation(om, save, file, sizeof file)) return;
    std::string opname = opsave::operation_name(save);
    known_path(kpath, sizeof kpath, opname);
    opsave::Known known;
    opsave::read_known(kpath, opname, &known);
    take_intel(save, kpath, opname, &known);
    unsigned added = opsave::learn_fought(save, &known);
    if (added) opsave::write_known(kpath, opname, known);
    mp_log("map: %u more of the host's battle groups have fought %s - identified for him from now on (%zu in all)", added, peer, known.size());
}

// Host, every frame on the operational map: share the map when the other player is ready, carry out a
// requested end of turn, give up on a player who does not answer.
static void host_map_frame(ObjMan *om, DWORD now)
{
    if (g_role != ROLE_HOST || g_tactical || g_host_mode != HOST_MODE_OPERATION) return;
    bool on_map = operation_on_screen(om) != 0;
    if (!on_map) { g_map_seen_since = 0; return; }
    if (!g_map_seen_since) g_map_seen_since = now;
    bool settled = now - g_map_seen_since > 4000;

    static bool learned;
    if (!learned && settled && !g_end_turn_pending && !g_turn_fix_at) { learned = true; host_learn_from_battles(om); }
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
        unsigned n = opsave::restore_tags(save, sidecar), total = 0, seen = 0, known = 0;
        opsave::knowledge(save, &total, &seen, &known);
        mp_log("map: after the turn the host sees %u of the other player's %u battle groups, has identified %u (the game's own scouting)", seen, total, known);
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
    // A copy for the other player: he is ready (in the lobby, or on an older copy of the map) and has none of
    // this state yet. While orders are given it is his to move his forces on; while the turn's battles are
    // fought it is his to look at (he goes into each battle from it). When the battles begin or end the
    // operation has moved on: his copy is of the past, and he is given the new one. (The stage is read only
    // once it has held for 3 s: while an operation's map is put up it passes through other values.)
    static int battles_raw = -1;
    static DWORD battles_since;
    int battles = battles_pending(om) ? 1 : 0;
    if (battles != battles_raw) { battles_raw = battles; battles_since = now; }
    bool steady = now - battles_since > 3000;
    if (settled && steady && g_battles_seen >= 0 && battles != g_battles_seen && g_map_given && !g_map_collect && !g_map_restart_at) {
        mp_log("map: the turn's battles have %s - the other player gets the map as it is now", battles ? "begun" : "all been fought");
        map_moved_on();
    }
    if (settled && steady) g_battles_seen = battles;
    if (settled && steady && !g_map_given && !g_map_collect && !g_map_restart_at && g_connected && g_peer_lobby_ready &&
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
    if (!g_tactical) { SetEnvironmentVariableA("MP_SWAPPED", 0); SetEnvironmentVariableA("MP_BATTLE_SETUP", 0); }
    g_map_mode = !g_tactical && GetEnvironmentVariableA("MP_MAP", v, sizeof v) && atoi(v);
    if (g_map_mode) {           // a copy to look at (set again for every map program; the way back from a battle is set on entering it)
        g_map_view = GetEnvironmentVariableA("MP_MAP_VIEW", v, sizeof v) && atoi(v);
        SetEnvironmentVariableA("MP_MAP_VIEW", 0);
        for (const char *e : { "MP_MAP_RETURN", "MP_MAP_RETURN_EPOCH", "MP_MAP_RETURN_PEER" }) SetEnvironmentVariableA(e, 0);
        if (g_map_view) mp_log("map: this copy is to look at - the host fights the turn's battles; the newest map replaces it");
    }
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
    g_upnp_on = GetEnvironmentVariableA("MP_UPNP", v, sizeof v) && atoi(v);
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
        // "steam:<id>": the host is reached through Steam, not at an address
        u64 by_steam = !_strnicmp(peer, "steam:", 6) ? _strtoui64(peer + 6, 0, 10) : 0;
        if (by_steam) strcpy_s(peer, "127.0.0.1");
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
        if (by_steam) {
            if (!steam_ready()) mp_log("net: the host is to be reached through Steam, and Steam is not there");
            g_peer = steam_addr(by_steam);
            steam_accept(by_steam);
        }
        if (open_socket(0)) {
            g_have_peer = true;
            if (by_steam) mp_log("net: joining through Steam (steam:%llu)", by_steam);
            else mp_log("net: joining %s:%u", peer, port);
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
    if (g_role == ROLE_HOST) steam_host(false, 0);
    g_have_peer = g_connected = g_bind_pending = false;
}

// ---------------------------------------------------------------- the engine's battle timer (base_main, iCtrlTimer)
// Its fields, as its constructor reads them from the program (beta16 base_main+0x3e20; 2026 beta +0x7ec0, where the
// class grew and every field moved - +0x108 is a link to i_render there):
#define CT_MULT BF(0x108, 0x110)       // float f:mult, the speed set (slot 3 sets it)
#define CT_CAP BF(0x10c, 0x114)        // float, the automatically reduced speed (moves towards the frame-rate table's)
#define CT_QUANT BF(0x114, 0x11c)      // float f:max_quant: no call takes more real time than this
#define CT_SAMPLES BF(0x158, 0x164)    // u32: frame times in its 16-frame history
#define CT_PAUSE BF(0x15c, 0x1dc)      // byte b:pause (slots 5 / 6 set and read it)
#define CT_AUTO BF(0x15d, 0x1dd)       // byte b:auto_tm_ctrl, the automatic speed control
#define CT_RENDER BF(NP, 0x108)        // 2026 beta: i_render, told "paused" by every call that runs at no speed

// ---------------------------------------------------------------- owned units: send transforms
// The steering of a vehicle's front wheels is its driver's, and nothing brought it over: in the other game
// that game's own driver code steered the other player's halftracks and lorries wherever it would have gone
// - the front wheels were seen turning aside and snapping back when a saved state came in. The driver
// (adv_physic+0x2b0a0, every step) moves the vehicle's steering command, +0x1230, towards what it wants by
// at most 1 a second and within +-0.5 (+-0.6 for some); the physics then turns each wheel that steers to
// that command times the wheel's own factor (adv_physic+0x8e690: wheel +0x1c4 = wheel +0x1c8 * +0x1230).
// The owner's command travels in UnitRec.pad: pad[0] in 1/180 (+-0.7), pad[1] bit 0 set when it is there
// (older builds send zeros), and is put into the vehicle before every step here - this game's driver can
// then turn the wheels by no more than a step's worth (0.02) before they are drawn.
// 2026 beta: the command is +0x1750 - the driver +0x2e5b0 moves it, and the wheel physics +0xf0630 does
// wheel +0x1c4 = wheel +0x1c8 * +0x1750 for the wheels that steer (wheels 0x2e0 bytes at +0x1708 there).
#define TECHN_STEER BF(0x1230, 0x1750)
static float vehicle_steer(const char *u)
{
    float s = *(const float *)(u + TECHN_STEER);
    return s == s && s > -1 && s < 1 ? s : 0;
}

static void put_steer(UnitRec &r, float steer)
{
    int q = (int)lroundf(steer * 180.0f);
    r.pad[0] = (unsigned char)(signed char)(q < -127 ? -127 : q > 127 ? 127 : q);
    r.pad[1] |= 1;
}

static bool get_steer(const UnitRec &r, float *steer)
{
    *steer = (signed char)r.pad[0] / 180.0f;
    return (r.pad[1] & 1) != 0;
}

static void steer_wheels(char *u, float steer)
{
    *(float *)(u + TECHN_STEER) = steer;
}

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
        // The engine's timer (base_main, iCtrlTimer): CT_MULT speed setting, CT_CAP the automatically reduced
        // speed, used only while CT_AUTO (automatic control) is on and its 16-frame history (CT_SAMPLES) is full.
        float set = timer ? *(float *)((char *)timer + CT_MULT) : 1, cur = timer ? *(float *)((char *)timer + CT_CAP) : 1;
        bool reduced = timer && ((char *)timer)[CT_AUTO] && *(u32 *)((char *)timer + CT_SAMPLES) >= 16 && cur < set;
        sh->speed = reduced ? cur : set;
        sh->cur_speed = set;
        sh->paused = timer ? ((char *)timer)[CT_PAUSE] : 0;
        sh->phase = (unsigned char)g_phase;
        sh->ready = (unsigned char)g_my_ready;
        sh->pad = 0;
        sh->want_speed = g_my_speed;
        sh->want_pause = g_my_pause;
        memset(sh->pad2, 0, sizeof sh->pad2);
        sh->pad2[0] = (unsigned char)(g_my_fps < 1 ? 1 : g_my_fps > 255 ? 255 : (int)(g_my_fps + 0.5f));      // frames a second here
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
        if (r.kind == 1) put_steer(r, vehicle_steer(u));
        recs[n++] = r;
        g_stat_sent++;
        sent++;
        if (n == k_units_per_packet) flush();
    }
    // the clock always goes out: the joining game follows the host's speed, pause and phase, and the host
    // waits for a joining game that has fallen behind
    if (n || !sent) flush();
}

// ---------------------------------------------------------------- the other player's vehicles: running gear
// A vehicle's wheels and tracks turn because its own drive turns them. In this game the other player's
// vehicles are not driven - they are put where their owner has them, with his velocities - so their wheels
// stood still and their tracks did not run. They are turned here, by battle time, the way the engine's
// drive works the ground speeds out (adv_physic+0x906d0): a wheel's ground speed is the hull's velocity at
// the wheel, (lin + w x (wheel - hull)) along the wheel's rolling direction; a track runs at the mean ground
// speed of its wheels that bear on the ground, and its wheels turn with it.
//   vehicle +0x11e8: wheels (0x2a0 bytes each, count +0x11f4): +0x30 position, +0x100 rolling direction,
//     +0x130 / +0x138 contact and the body it belongs to, +0x17c radius, +0x1b0 angle (what is drawn),
//     +0x1b4 spin, +0x289 / +0x292 bearing flags;
//   vehicle +0x1200: tracks (0x2c0 bytes, count +0x120c): +0x16c and +0x1a4 how far the track has run,
//     +0x200 its wheels (16-byte entries, the wheel first), count +0x20c.
static float wheel_ground_speed(const char *w)
{
    const char *b = *(const char *const *)(w + 0x138);
    if (!readable(b, 0x1f0)) return 0;
    const float *bp = (const float *)(b + 0x1b0), *lin = (const float *)(b + 0x1d0), *ang = (const float *)(b + 0x1e0);
    const float *r0 = (const float *)(b + 0xf0), *r1 = (const float *)(b + 0x100), *r2 = (const float *)(b + 0x110);
    float wx = ang[0] * r0[0] + ang[1] * r1[0] + ang[2] * r2[0];
    float wy = ang[0] * r0[1] + ang[1] * r1[1] + ang[2] * r2[1];
    float wz = ang[0] * r0[2] + ang[1] * r1[2] + ang[2] * r2[2];
    const float *wp = (const float *)(w + 0x30), *ax = (const float *)(w + 0x100);
    float dx = wp[0] - bp[0], dy = wp[1] - bp[1], dz = wp[2] - bp[2];
    return ((dy * wx - dx * wy) + lin[2]) * ax[2] + ((dz * wy - dy * wz) + lin[0]) * ax[0] + ((dx * wz - dz * wx) + lin[1]) * ax[1];
}

static bool wheel_bears(const char *w)
{
    return w[0x292] && w[0x289] && *(const void *const *)(w + 0x130) && *(const void *const *)(w + 0x138);
}

static void turn_wheel(char *w, float speed, float dt)
{
    float r = *(float *)(w + 0x17c);
    if (!(r > 0.05f) || !(speed == speed)) return;
    float *angle = (float *)(w + 0x1b0);
    *angle += speed / r * dt;
    if (*angle > 1000.0f || *angle < -1000.0f) *angle = fmodf(*angle, 6.2831853f);
}

// 2026 beta: not ported. Its wheels are 0x2e0-byte records at +0x1708 (count +0x1714; adv_physic+0x127a0 reads
// them), part of their fields moved (the bearing flag +0x289 is +0x2c1) and the tracks are laid from links
// (+0x50990) - the fields this needs are not all mapped there, so the other player's running gear stands still.
#define TECHN_WHEELS BF(0x11e8, NP)
static void turn_running_gear(char *u, float dt)
{
    if (!TECHN_WHEELS) {
        static bool said;
        if (!said) { said = true; mp_log("net: running gear: not ported to this build, the other player's wheels and tracks are not turned"); }
        return;
    }
    char *wheels = *(char **)(u + 0x11e8), *tracks = *(char **)(u + 0x1200);
    u32 nw = *(u32 *)(u + 0x11f4), nt = *(u32 *)(u + 0x120c);
    if (nw > 64 || nt > 8 || (nw && !readable(wheels, (size_t)nw * 0x2a0)) || (nt && !readable(tracks, (size_t)nt * 0x2c0))) return;
    char *on_track[64];
    u32 n_on = 0;
    for (u32 t = 0; t < nt; t++) {
        char *tr = tracks + (size_t)t * 0x2c0;
        char *list = *(char **)(tr + 0x200);
        u32 n = *(u32 *)(tr + 0x20c);
        if (!n || n > 64 || !readable(list, (size_t)n * 16)) continue;
        float sum = 0;
        int bearing = 0;
        for (u32 k = 0; k < n; k++) {
            char *w = *(char **)(list + (size_t)k * 16);
            if (readable(w, 0x2a0) && wheel_bears(w)) { sum += wheel_ground_speed(w); bearing++; }
        }
        if (!bearing) continue;
        float speed = sum / bearing;
        if (!(speed == speed)) continue;
        *(float *)(tr + 0x16c) += speed * dt;
        *(float *)(tr + 0x1a4) += speed * dt;
        for (u32 k = 0; k < n; k++) {
            char *w = *(char **)(list + (size_t)k * 16);
            if (!readable(w, 0x2a0)) continue;
            turn_wheel(w, speed, dt);
            if (n_on < 64) on_track[n_on++] = w;
        }
    }
    // Wheels that run on no track (a wheeled vehicle, a halftrack's front wheels): each by its own ground
    // speed. These have no contact record (+0x130 is empty in their owner's game too); the engine turns
    // them by their spin (+0x1b4), which here only came with a loaded state and died away within half a
    // second - the wheels stopped and started. The spin is taken out and the wheel turned like the others.
    for (u32 i = 0; i < nw; i++) {
        char *w = wheels + (size_t)i * 0x2a0;
        bool done = false;
        for (u32 k = 0; k < n_on && !done; k++) done = on_track[k] == w;
        if (done || !w[0x292] || !w[0x289] || !*(const void *const *)(w + 0x138)) continue;
        turn_wheel(w, wheel_ground_speed(w), dt);
        *(float *)(w + 0x1b4) = 0;
    }
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
    g_sim_steps_seen++;
    double t0 = seconds_now();
    // (a state is only loaded once the other game's numbers can be translated - see xlate.cpp)
    if (sync_state_enabled() && xlate_ready() && GetTickCount() - first_step >= (DWORD)delay) statesync_apply(om);
    g_prof_load += seconds_now() - t0;
    u32 count;
    all_objects(om, &count);
    if (count != g_units_count_seen) rebuild_units(om);
    shots_sim_step(om);
    hits_sim_step(om);
    trench_sim_step(om);
    anim_sim_step(om);
    vdamage_sim_step(om);

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
    // In the battle phase the transforms are placed by their owner's battle time, not by when they arrived.
    // A transform is taken when its owner's game draws a frame and is looked at when this game draws one,
    // so the time it arrived is off by up to a frame of each game; blending by it moved the other player's
    // units in fits and starts - a soldier walking a steady 4.1 m/s was shown covering 1.7 to 7.5 m/s from
    // one frame to the next, and the slower the other game drew, the worse. Each transform carries its
    // owner's battle time; that clock is followed here against this PC's own (g_play: offset and rate, as
    // the two battle clocks drift apart by a tenth while the joining game catches up - this game's own
    // battle time cannot stand in for it), and a unit is shown where its owner had it at that clock now,
    // less a delay that covers the gap between two transforms and how much their arrivals wander. Outside
    // the battle phase the battle clocks stand still and the time of arrival is used, as before.
    double now_s = seconds_now();
    for (const Pending &pe : g_pending) {
        const UnitRec &r = pe.rec;
        u64 key = ((u64)r.kind << 32) | r.id;
        auto it = g_units.find(key);
        if (it == g_units.end() || owns_object(it->second)) continue;
        Held &h = g_held[key];
        Snap nw = { r, now_s, pe.sim };
        Snap *last = h.n ? &h.s[h.n - 1] : 0;
        if (last && pe.sim >= 0 && last->sim >= 0) {
            if (pe.sim < last->sim) continue;                                  // (an older one, overtaken on the way)
            if (pe.sim == last->sim) { *last = nw; continue; }
        } else if (last && now_s - last->at <= 0.004) { *last = nw; continue; }      // (same step: just the newer one)
        if (h.n == HELD_SNAPS) { memmove(h.s, h.s + 1, (HELD_SNAPS - 1) * sizeof(Snap)); h.n--; }
        h.s[h.n++] = nw;
    }
    g_pending.clear();

    const double k_delay = 0.10;
    // How far apart two transforms may be and still be blended, and how long a unit runs on past the newest:
    // in battle seconds, so they grow with the battle's speed - at x3 transforms come ~0.27 battle s apart and
    // the fixed 0.6 / 0.25 of normal speed made units jump to the later transform, or stand and then jump
    // (3-5 m, about 80 units in 5 s in a skirmish fight). MP_FOLLOW_FIXED=1: the fixed values as before.
    static int fixed_limits = -1;
    if (fixed_limits < 0) { char v[8]; fixed_limits = GetEnvironmentVariableA("MP_FOLLOW_FIXED", v, sizeof v) && atoi(v); }
    bool scaled = !fixed_limits && g_phase == 2 && g_play.have;
    const double k_gap = scaled ? max(0.6, 4.0 * g_play.interval) : 0.6;
    const double k_run_on = scaled ? max(0.25, 2.5 * g_play.interval + 2.0 * g_play.dev) : 0.25;
    static int log_jumps = -1, jumps_logged;
    if (log_jumps < 0) { char v[8]; log_jumps = GetEnvironmentVariableA("MP_TEST_JUMPS", v, sizeof v) && atoi(v); }
    bool by_battle_time = g_phase == 2 && g_play.have;
    // The delay changes by at most a sixth of the time passing (the shown clock runs a sixth slower or faster
    // for a while): put to a new delay at once, a burst of late transforms - the delay up from 0.3 to 1.5 s -
    // put every unit a second back on its way and then forward again.
    static double shown_delay = -1, delay_at;
    {
        double want = play_delay(), since = now_s - delay_at;
        delay_at = now_s;
        if (shown_delay < 0 || !by_battle_time || since > 1.0 || since < 0) shown_delay = want;
        else {
            double most = since * max(g_play.rate, 1.0) / 6;
            shown_delay += want - shown_delay > most ? most : want - shown_delay < -most ? -most : want - shown_delay;
        }
    }
    double play_t = play_clock(now_s) - shown_delay;
    // (the running gear turns by the battle time this step covers - none in a frame without a step)
    static u64 last_gear_ms;
    float gear_dt = g_sim_ms >= last_gear_ms ? (float)min((g_sim_ms - last_gear_ms) / 1000.0, 0.1) : 0.0f;
    last_gear_ms = g_sim_ms;
    for (auto &kv : g_held) {
        Held &h = kv.second;
        auto it = g_units.find(kv.first);
        if (it == g_units.end() || owns_object(it->second)) continue;
        if (!h.n) continue;
        // (by battle time only with transforms taken in the battle; a unit whose newest one is from before
        //  it stands where that one has it)
        const Snap &newest = h.s[h.n - 1];
        bool sim = by_battle_time && newest.sim >= 0;
        double t = sim ? play_t : now_s - k_delay;
        auto when = [&](const Snap &x) { return sim ? x.sim : x.at; };
        // the two transforms around t: the newest pair whose older one is not after it
        int j = 0;              // index of the later one of the pair, 0: none
        double ta = 0, tb = when(newest);
        for (int i = h.n - 1; i >= 1; i--) {
            const Snap &a = h.s[i - 1];
            if (sim && a.sim < 0) break;
            double wa = when(a), wb = when(h.s[i]);
            // a unit that stood still was last sent seconds ago: it set off from there just before the next one
            if (sim && wb - wa >= k_gap) wa = wb - min(max(g_play.interval, 0.03), k_gap / 2);
            if (t >= wa || i == 1) { j = i; ta = wa; tb = wb; break; }
        }
        float pos[3], rot[9];
        const UnitRec &B = j ? h.s[j].rec : newest.rec;
        bool moving = true;
        float steer_k = 1;
        int branch = 0;         // (for the jump log) 0 standing by arrival, 1 blended, 2 run on / stand
        if (by_battle_time && !sim) {
            memcpy(pos, B.pos, sizeof pos);
            memcpy(rot, B.rot, sizeof rot);
            moving = false;
        } else if (j && t < tb && tb - ta > 0.001 && tb - ta < k_gap) {
            branch = 1;
            const UnitRec &A = h.s[j - 1].rec;
            float k = t <= ta ? 0.0f : (float)((t - ta) / (tb - ta));
            steer_k = k;
            for (int i = 0; i < 3; i++) pos[i] = A.pos[i] + (B.pos[i] - A.pos[i]) * k;
            for (int i = 0; i < 9; i++) rot[i] = A.rot[i] + (B.rot[i] - A.rot[i]) * k;
        } else {
            branch = 2;
            double run = t - tb;
            run = run < 0 ? 0 : run > k_run_on ? k_run_on : run;
            moving = t - tb < k_run_on;
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
        // Tests: MP_TEST_TRACE=1 logs, for one soldier on the move, where each step puts him by battle time and
        // where the time of arrival would have put him, for 8 s.
        static int trace = -1;
        static u64 trace_key, trace_until;
        if (trace < 0) { char v[8]; trace = GetEnvironmentVariableA("MP_TEST_TRACE", v, sizeof v) && atoi(v) > 0; }
        if (trace && sim && h.n >= 2 && !strcmp(it->second->iface, "i_human")) {
            if (!trace_key && B.lin[0] * B.lin[0] + B.lin[2] * B.lin[2] > 2.0f) { trace_key = kv.first; trace_until = g_sim_ms + 8000; }
            if (kv.first == trace_key && g_sim_ms < trace_until) {
                // (the old way: the last two, by when they arrived)
                const Snap &oa = h.s[h.n - 2], &ob = h.s[h.n - 1];
                double to = now_s - k_delay, a0 = oa.at, b0 = ob.at;
                float old[3];
                const UnitRec &A = oa.rec, &OB = ob.rec;
                if (to < b0 && b0 - a0 > 0.001 && b0 - a0 < 0.6) {
                    float k = to <= a0 ? 0.0f : (float)((to - a0) / (b0 - a0));
                    for (int i = 0; i < 3; i++) old[i] = A.pos[i] + (OB.pos[i] - A.pos[i]) * k;
                } else {
                    double run = min(max(to - b0, 0.0), k_run_on);
                    for (int i = 0; i < 3; i++) old[i] = OB.pos[i] + OB.lin[i] * (float)run;
                }
                const float *up = (const float *)(u + UNIT_POS), *ul = (const float *)(u + UNIT_LIN);
                float mx = up[0] - h.wrote[0], mz = up[2] - h.wrote[2];
                mp_log("trace: battle %.3f shown %.3f real %.4f  by battle time %.3f %.3f  by arrival %.3f %.3f  %s pair %.3f..%.3f newest %.3f;"
                       " since the last step this game moved him %.3f %.3f in %llu ms (velocity written %.2f %.2f, now %.2f %.2f); this step moves him %.3f",
                       g_sim_ms / 1000.0, t, now_s, pos[0], pos[2], old[0], old[2], branch == 1 ? "blend" : branch == 2 ? "past" : "arr",
                       ta, tb, when(newest), mx, mz, g_sim_ms - h.wrote_ms, h.wrote_lin[0], h.wrote_lin[2], ul[0], ul[2],
                       sqrtf((pos[0] - up[0]) * (pos[0] - up[0]) + (pos[2] - up[2]) * (pos[2] - up[2])));
            }
        }
        // A soldier who sits on a vehicle in this game is placed by that vehicle (his seat), every step. Whether
        // a soldier rides is not the same in the two games during deployment and the initial orders: in a test
        // skirmish soldiers seated here were pulled 44-91 m to where their owner had them, 50 times a second, and
        // put back in their seats by the vehicle each time - and at the end of deployment, when the game seats
        // and unseats soldiers, the host's game crashed twice in an operation battle (adv_physic+0x749b0,
        // finding a seat's point). Such a soldier is left to ride with his vehicle; he follows his owner again
        // as soon as he is on foot here.
        if (!strcmp(it->second->iface, "i_human") && readable(u, SEAT_VEHICLE + 8) && *(void **)(u + SEAT_VEHICLE) && *(int *)(u + SEAT_STATION) != -1) {
            g_stat_seated++;
            continue;
        }
        float *upos = (float *)(u + UNIT_POS), *urot = (float *)(u + UNIT_ROT), *uinv = (float *)(u + UNIT_INV);
        float e[3] = { pos[0] - upos[0], pos[1] - upos[1], pos[2] - upos[2] };
        float err = sqrtf(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
        g_stat_err_sum += err;
        if (err > g_stat_err_max) g_stat_err_max = err;
        if (err > 3.0f) {
            g_stat_snaps++;
            if (log_jumps && jumps_logged < 80 && g_phase == 2 && err < 100) {
                jumps_logged++;
                mp_log("jump: %s %.1f m - here %.2f %.2f, now %.2f %.2f; %s; t %.3f pair %.3f..%.3f (newest %.3f, %d held), spacing %.3f, gap limit %.2f, run-on %.2f",
                       it->second->full, err, upos[0], upos[2], pos[0], pos[2], branch == 1 ? "blended" : branch == 2 ? "past the pair (run on / stand)" : "by arrival",
                       t, ta, tb, when(newest), h.n, g_play.interval, k_gap, k_run_on);
            }
            JumpStat &js = g_jumps[kv.first];
            js.n++;
            if (err > js.max) js.max = err;
            js.o = it->second;
            memcpy(js.here, upos, 12);
            memcpy(js.there, pos, 12);
        }
        // A vehicle standing still is left where this game's physics has it while that is within 25 cm and about
        // two degrees of its owner's place: put back every step, it was lifted again each time by this game's
        // suspension, which rests it a few centimetres higher or lower - a parked tank seen bouncing in place
        // (6.5 cm, 40 times a second). Further off, or moving, it is put where its owner has it as before.
        // MP_FOLLOW_REST=0: every step.
        static int rest_on = -1;
        if (rest_on < 0) { char v[8]; rest_on = !GetEnvironmentVariableA("MP_FOLLOW_REST", v, sizeof v) || atoi(v) != 0; }
        const UnitRec &Aprev = j ? h.s[j - 1].rec : B;
        float lin2 = B.lin[0] * B.lin[0] + B.lin[1] * B.lin[1] + B.lin[2] * B.lin[2];
        float da[3] = { B.pos[0] - Aprev.pos[0], B.pos[1] - Aprev.pos[1], B.pos[2] - Aprev.pos[2] };
        bool still = !moving || (lin2 < 0.0025f && da[0] * da[0] + da[1] * da[1] + da[2] * da[2] < 0.0004f);
        float fwd = urot[0] * rot[0] + urot[1] * rot[1] + urot[2] * rot[2], up = urot[4] * rot[3] + urot[5] * rot[4] + urot[6] * rot[5];
        if (rest_on && B.kind == 1 && still && err < 0.25f && fwd > 0.9994f && up > 0.9994f) {
            g_stat_rest++;
            continue;
        }
        g_stat_applied++;
        memcpy(upos, pos, 12);
        for (int row = 0; row < 3; row++)
            for (int col = 0; col < 3; col++) {
                urot[row * 4 + col] = rot[row * 3 + col];
                uinv[col * 4 + row] = rot[row * 3 + col];
            }
        if (moving) { memcpy(u + UNIT_LIN, B.lin, 12); memcpy(u + UNIT_ANG, B.ang, 12); }
        else { memset(u + UNIT_LIN, 0, 12); memset(u + UNIT_ANG, 0, 12); }
        memcpy(h.wrote, pos, 12);
        memcpy(h.wrote_lin, u + UNIT_LIN, 12);
        h.wrote_ms = g_sim_ms;
        h.branch = branch; h.k = steer_k; h.t = t; h.ta = ta; h.tb = tb; h.newest = when(newest);
        float sb, sa;
        if (B.kind == 1 && get_steer(B, &sb))
            steer_wheels(u, steer_k < 1 && j && get_steer(h.s[j - 1].rec, &sa) ? sa + (sb - sa) * steer_k : sb);
        if (B.kind == 1 && g_phase == 2) turn_running_gear(u, gear_dt);
    }
    guns_sim_step(om);
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
// 2026 beta: the same three, at +0x3e340, +0x3e480 and +0x489b0, on the moved fields (deployment +0x1149 and
// +0x114a, the open box +0x1054 instead of +0xed9, +0xeda, +0xde8).
#define IE_BOX BF(0xde8, 0x1054)
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
    *(u32 *)((char *)ui + IE_BOX) = 6;
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
    // +0xde8 (IE_BOX): which box is open. A yes to "end deployment?" becomes a no for the engine (the box closes,
    // nothing else happens) and a ready mark for us.
    if (gate_active() && g_phase == 0 && result && *(u32 *)((char *)ui + IE_BOX) == 3) {
        mark_ready();
        result = 0;
    }
    // box 0 answered yes = the ESC menu's Withdraw: what it does to the battle's end is this player's own doing
    bool withdraw = result && *(u32 *)((char *)ui + IE_BOX) == 0;
    if (withdraw) endgame_manual(true);
    g_box_result(ui, result, c, d);
    if (withdraw) endgame_manual(false);
}

// The game's own ESC menu: i_intereff, adv_interf+0x228c0(interface, show) opens or closes it (2026 beta
// +0x241a0, which begins with the same bytes).
void session_game_menu(ObjMan *om, bool show)
{
    static const unsigned char sig[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x56, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0x01, 0x0f, 0xb6, 0xf2 };
    char *fn = build_code("adv_interf.codelib", BV(0x228c0, 0x228c0, 0x241a0), sig, sizeof sig);
    Object *ui = engine_find(om, "i_intereff", "intereff");
    if (!fn || !ui || !g_tactical || (*((char *)ui + IE_MENU_OPEN) != 0) == show) return;
    ((void (*)(Object *, char))fn)(ui, show);
}

// Tests: the ESC menu opened or closed, and its Withdraw button pressed and confirmed (the menu opens, the
// game's "leave the battle?" box - number 0 - is answered yes, which closes the menu again).
static int g_test_menu = -1;
static bool g_withdraw_asked, g_pause_asked;
void session_test_menu(bool show) { g_test_menu = show; }
void session_test_withdraw(void) { g_withdraw_asked = true; }
void session_test_pause(void) { g_pause_asked = true; }      // the pause key
void session_test_time(void) { mp_log("test: battle time %.2f s", g_sim_ms / 1000.0); }
static int g_speed_asked = -100;
void session_test_speed(int level) { g_speed_asked = level; }  // the speed keys: level -3 .. 10, 0 = normal

static void menu_tests(ObjMan *om)
{
    if (g_test_menu >= 0) {
        mp_log("test: the game's menu is %s", g_test_menu ? "opened" : "closed");
        session_game_menu(om, g_test_menu != 0);
        g_test_menu = -1;
    }
    if (g_speed_asked > -100) {
        Object *gs = engine_find(om, "i_gamespec", "gamespec");
        if (gs && g_tactical && g_phase == 2) {
            mp_log("test: speed level %d", g_speed_asked);
            // (i_gamespec slot 20, set_time_level: beta16 +0xff5a0, 2026 beta +0x183370 - the same slot)
            ((void (*)(Object *, int))gs->vtbl[20])(gs, g_speed_asked);
        }
        g_speed_asked = -100;
    }
    if (g_pause_asked) {
        g_pause_asked = false;
        Object *ui = engine_find(om, "i_intereff", "intereff");
        if (ui && g_tactical && g_start_action && g_phase == 2) { mp_log("test: pause key"); g_start_action(ui, 0, 0, 0); }
    }
    if (!g_withdraw_asked || !g_tactical || !g_box_result) return;
    g_withdraw_asked = false;
    Object *ui = engine_find(om, "i_intereff", "intereff");
    if (!ui || g_phase != 2) return;
    mp_log("test: Withdraw pressed in the ESC menu and confirmed (box 0)");
    session_game_menu(om, true);
    *(u32 *)((char *)ui + IE_BOX) = 0;
    box_result_hook(ui, 1, 0, 0);
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

// 4. While the battle runs with another player in it, its clock is both players'. The interface's pause
//    and speed keys do not act on the clock any more; they are this player's WORD:
//      - the battle pauses when both players have asked for a pause, and goes on when either lets go;
//      - it runs faster than normal only as fast as both have asked for (the slower of the two wishes), and
//        slower than normal only as far as both have asked for; with one wish alone it runs at normal speed.
//    Each game sends its player's word with its unit transforms (StateHead.want_*); the host's clock is set
//    from the two, and the joining game follows the host's clock as before.
//    The game also pauses by itself: when the ESC menu opens (adv_interf+0x229c0 - and the stock game stays
//    paused when it closes) and when the box of an event is opened (+0x19d00). Those are nobody's word and do
//    nothing here: a player in his menu no longer stops the battle for the other one.
//    The keys switch on "is it paused?" (adv_interf+0x3b3b0 .. +0x3ba00 read the timer's flag and set the
//    opposite), so inside those routines the answer is this player's own word, not the clock's state.
//    (2026 beta: the same routines at +0x242a0 .. +0x24490, +0x1b650 .. +0x1b930 and +0x3e340 .. +0x3e990 - the
//    start action, the Start button, three helpers - each calling the timer's slots 5 and 6 as before.)
static bool shared_clock(void)
{
    return g_tactical && g_role != ROLE_NONE && g_connected && g_phase == 2 && !endgame_over();
}

static float agreed_speed(void)
{
    float a = g_my_speed, b = g_peer_speed;
    if (a > 1 && b > 1) return a < b ? a : b;
    if (a < 1 && b < 1) return a > b ? a : b;
    return 1;
}

static bool agreed_pause(void) { return g_my_pause && g_peer_pause; }

// 6. Faster than normal the battle works up to the agreed speed, as the stock game's own speed control does
//    (a quarter of the difference a second), and stops working up while either game draws 30 frames a second
//    or fewer: each step of speed makes every frame carry more simulation, so the frame rate is what tells
//    when a PC has had enough. It goes on working up once both draw more again; it does not slow down by
//    itself. Normal speed, slower than normal, and slowing down take effect at once. The host's clock does
//    this; the joining game follows it.
static const char *slow_game(float *fps, const char *peer)
{
    static char who[96];
    if (g_my_fps > 0 && g_my_fps <= 30) { *fps = g_my_fps; return "your game"; }
    if (g_peer_fps > 0 && g_peer_fps <= 30 && GetTickCount() - g_peer_fps_at < 3000) {
        *fps = g_peer_fps;
        sprintf_s(who, "%s's game", peer && *peer ? peer : "the other player");
        return who;
    }
    return 0;
}

static float running_speed(void) { return g_role == ROLE_HOST ? g_ramp : g_host_speed; }

static void ramp_speed(float dt)
{
    float want = agreed_speed();
    static bool held;
    if (want <= 1 || want <= g_ramp) { g_ramp = want; held = false; return; }
    if (agreed_pause() || !(dt > 0)) return;
    float fps;
    const char *slow = slow_game(&fps, g_peer_name);
    if ((slow != 0) != held) {
        held = slow != 0;
        if (held) mp_log("clock: speed held at x%.2f on the way to x%.1f - %s draws %.0f frames a second", g_ramp, want, slow, fps);
        else mp_log("clock: working up to x%.1f again from x%.2f", want, g_ramp);
    }
    if (held) return;
    g_ramp += 0.25f * (want - g_ramp) * (dt < 0.25f ? dt : 0.25f);
    if (want - g_ramp < 0.02f) g_ramp = want;
}

static bool called_from(void *ret, size_t from, size_t to)
{
    static char *interf;
    if (!interf) interf = build_module("adv_interf.codelib");
    return interf && from && to && (char *)ret >= interf + from && (char *)ret < interf + to;
}

static void set_speed_hook(char *ct, float speed)
{
    if (!shared_clock()) { g_set_speed(ct, follows_host_clock() ? g_host_speed : speed); return; }
    if (speed != g_my_speed) {
        g_my_speed = speed;
        mp_log("clock: this player asks for speed x%.2f", speed);
    }
    g_set_speed(ct, g_role == ROLE_JOIN ? (g_have_host_clock ? g_host_speed : 1.0f) : g_ramp);
}

static void set_pause_hook(char *ct, unsigned char paused)
{
    if (!shared_clock()) { g_set_pause(ct, follows_host_clock() ? g_host_paused : paused); return; }
    void *ret = _ReturnAddress();
    bool nobodys = g_stepping_phase || called_from(ret, BV(0x229c0, 0x229c0, 0x242a0), BV(0x22bb0, 0x22bb0, 0x24490)) ||
                   called_from(ret, BV(0x19d00, 0x19d00, 0x1b650), BV(0x19fb0, 0x19fb0, 0x1b930));
    if (!nobodys && (paused != 0) != g_my_pause) {
        g_my_pause = paused != 0;
        mp_log("clock: this player %s", g_my_pause ? "asks for a pause" : "no longer asks for a pause");
    }
    g_set_pause(ct, g_role == ROLE_JOIN ? (unsigned char)(g_have_host_clock ? g_host_paused : 0) : (unsigned char)agreed_pause());
}

static unsigned char get_pause_hook(char *ct)
{
    if (shared_clock() && !g_stepping_phase && called_from(_ReturnAddress(), BV(0x3b3b0, 0x3b320, 0x3e340), BV(0x3ba00, 0x3b960, 0x3e990)))
        return g_my_pause;
    return g_get_pause(ct);
}

static void replace_timer_setters(ObjMan *om)
{
    static bool done;
    if (done || g_role == ROLE_NONE || !g_tactical) return;
    Object *timer = engine_find(om, "i_ctrltimer", "ctrltimer");
    char *base = build_module("base_main.codelib");
    if (!timer || !base) return;
    done = true;
    void **vt = *(void ***)timer;
    // (2026 beta: the class's table at base_main+0x36558, still 8 slots; set_mult +0x9230, set_pause +0x7d20 and
    //  is_paused +0x7d30 write and read b:pause at +0x1dc. Slot 3 returns whether the clock had stood - nobody reads it.)
    if (!BV(0x23bc8, 0x23bc8, 0x36558) || (char *)vt != base + BV(0x23bc8, 0x23bc8, 0x36558) || (char *)vt[3] != base + BV(0x4680, 0x4680, 0x9230) ||
        (char *)vt[5] != base + BV(0x3db0, 0x3db0, 0x7d20) || (char *)vt[6] != base + BV(0x3dc0, 0x3dc0, 0x7d30)) {
        mp_log("net: unknown game build, speed and pause stay local");
        return;
    }
    // a private copy of the class's function table for this one object (with the type information
    // pointer that sits just before it)
    static void *copy[9];
    memcpy(copy, vt - 1, sizeof copy);
    g_set_speed = (SetSpeed)vt[3];
    g_set_pause = (SetPause)vt[5];
    g_get_pause = (GetPause)vt[6];
    copy[1 + 3] = set_speed_hook;
    copy[1 + 5] = set_pause_hook;
    copy[1 + 6] = get_pause_hook;
    *(void ***)timer = copy + 1;
    mp_log("net: pause and speed are both players': the battle pauses or runs faster only when both ask for it%s",
           g_role == ROLE_JOIN ? " (this game's clock follows the host's)" : "");
}

// How much over a whole number of 5 ms a call to the timer is given. The 2024 timer drops what is left of the
// 5 ms (so 0.02f, just under 20 ms, would be 15): 0.1 ms over. The 2026 beta's carries it into the next call
// (base_main+0x8390, +0x120), where any hair adds up - there only enough to cover a float's rounding.
static double step_hair(void) { return g_bi == BUILD_BETA26 ? 1e-6 : 0.0001; }

static void timer_step_hook(char *ct, float dt, u32 arg)
{
    bool multi = g_tactical && g_connected && g_role != ROLE_NONE;
    double scale = 1;
    // Tests: MP_TEST_SPEED=n runs the battle n times as fast in real time, in steps of the usual size (the
    // clocks' margins below grow with it as with the game's own speed).
    static double hurry = -1;
    if (hurry < 0) {
        char v[16];
        hurry = GetEnvironmentVariableA("MP_TEST_SPEED", v, sizeof v) && atof(v) > 0 ? atof(v) : 1;
    }
    // 5. The engine's automatic speed control is off while another player is in the battle. It lives in
    //    this same step function: with the option b:auto_speed_ctrl (timer +0x15d, CT_AUTO) the speed actually used is
    //    not the one set (+0x108, CT_MULT) but +0x10c (CT_CAP), which creeps towards it by a quarter of the difference per
    //    second of frame time and is capped by the frame rate (a table: under 50 frames a second x5 ... under
    //    15 x0.75, under 10 x0.5). Stock, with one step per frame, that is a soft start. Here the battle is
    //    stepped in fixed 20 ms steps and the engine cuts speed x step down to whole 5 ms: a speed of 0.99
    //    makes every step 15 ms, so the battle ran at THREE QUARTERS of its speed for the half minute +0x10c
    //    needs to creep from 0.75 to 0.9995 - at the start of every battle, after every pause (a paused game
    //    under 50 frames a second lets +0x10c fall to zero), and a change of speed took a quarter of a
    //    minute to arrive. The joining game followed, since it runs on the host's clock.
    //    How fast the battle runs is the two players' decision alone (4.); a PC that cannot keep up is waited
    //    for by the clocks (3.), not slowed down for.
    //    (2026 beta, base_main+0x8390: the same control, and with it on the speed also eases in and out over
    //    f:start_time seconds around a pause; with it off neither happens.)
    static int auto_was = -1;
    if (g_role == ROLE_HOST && multi) {
        if (auto_was < 0) {
            auto_was = ct[CT_AUTO];
            mp_log("clock: the engine's automatic speed control is %s; off while the other player is in the battle", auto_was ? "on" : "off already");
        }
        ct[CT_AUTO] = 0;
        *(float *)(ct + CT_CAP) = *(float *)(ct + CT_MULT);
    } else if (auto_was >= 0 && g_role == ROLE_HOST) {
        ct[CT_AUTO] = (char)auto_was;         // alone again: as the player has it set
        auto_was = -1;
    }
    if (g_role == ROLE_HOST && g_set_speed && shared_clock()) {
        // the host's clock is what the two players have agreed on
        ct[CT_PAUSE] = agreed_pause();
        ramp_speed(dt);
        float want = g_ramp;
        if (*(float *)(ct + CT_MULT) != want) g_set_speed(ct, want);
        *(float *)(ct + CT_CAP) = want;
    }
    if (follows_host_clock()) {
        ct[CT_AUTO] = 0;                              // no automatic slow-down of its own
        *(float *)(ct + CT_MULT) = g_host_speed;
        *(float *)(ct + CT_CAP) = g_host_speed;
        ct[CT_PAUSE] = g_host_paused;
        if (g_phase == 2 && g_host_phase == 2) {
            double speed = (g_host_speed > 1 ? g_host_speed : 1) * hurry;
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
        double speed = (*(float *)(ct + CT_MULT) > 1 ? *(float *)(ct + CT_MULT) : 1) * hurry;
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
    if (!multi || g_phase != 2 || ct[CT_PAUSE] || g_sim_step <= 0) {
        due = 0;
        g_waiting_since = 0;
        g_timer_step(ct, dt, arg);
        return;
    }
    // A joining game that has to catch up may take a dozen steps in one frame; a host takes five at most
    // and otherwise lets its clock run slow (the joining game follows it anyway).
    double most = g_sim_step * (g_role == ROLE_JOIN ? 12 : 5) * hurry;
    due += dt * scale * hurry;
    if (due > most) due = most;
    double t0 = seconds_now();
    int steps = 0;
    // mpmod2: the joining game's simulation decides nothing (the host's game does), so it does not need the
    // host's fixed 20 ms steps - each of which is a whole frame's work for the engine (the soldiers' per-frame
    // update, the shells, the scene). At x3 with two games on one PC those steps took the joining game down to
    // 6 frames a second, a dozen steps a frame: units moved a few centimetres in the steps of one frame and
    // jumped metres to the next. So it takes one call a frame with all the time due, in whole 5 ms; the engine
    // cuts it into its own steps of at most 50 ms, as the stock game does. MP_JOIN_FIXED_STEPS=1: as the host.
    static int join_fixed = -1;
    if (join_fixed < 0) { char v[8]; join_fixed = GetEnvironmentVariableA("MP_JOIN_FIXED_STEPS", v, sizeof v) && atoi(v); }
    if (g_role == ROLE_JOIN && host_auth() && !join_fixed) {
        double take = floor(due / 0.005) * 0.005;
        if (take > 0.25 * hurry) take = 0.25 * hurry;
        if (take >= 0.005) {
            due -= take;
            // (the timer takes no more real time per call than +0x114 - 70 ms, a frame of 14 a second - and
            //  drops the rest: a frame of a 30-a-second game at MP_TEST_SPEED=3 is 100 ms, and the joining game
            //  ran at two thirds of the host's speed, which waited for it. Calls of whole 5 ms under it.
            //  2026 beta: f:max_quant is +0x11c, and the timer allows the larger of it and 1.25 median frames.)
            float cap = *(float *)(ct + CT_QUANT);
            double most_call = cap > 0.01f ? floor((cap - 0.0002) / 0.005) * 0.005 : take;
            if (most_call < 0.005) most_call = 0.005;
            int calls = (int)ceil(take / most_call - 1e-6);
            calls = calls < 1 ? 1 : calls > 8 ? 8 : calls;
            for (int i = 0; i < calls && take >= 0.005; i++) {
                // (even parts: 100 ms as 50 + 50, not 70 + 30)
                double c = i == calls - 1 ? take : floor(take / (calls - i) / 0.005 + 0.5) * 0.005;
                if (c > most_call) c = most_call;
                take -= c;
                g_timer_step(ct, (float)(c + step_hair()), arg);
                steps++;
            }
            due += take;            // (what eight calls did not cover)
        }
    }
    for (; due >= g_sim_step; steps++) {
        if (steps >= 3 && seconds_now() - t0 > 0.06 * hurry) break;     // keep the picture moving; the rest is taken next frame
        due -= g_sim_step;
        // a hair over the step: the timer cuts speed x time down to whole 5 ms, and 0.02f is just under 20
        g_timer_step(ct, (float)(g_bi == BUILD_BETA26 ? g_sim_step + step_hair() : g_sim_step * 1.0005), arg);
    }
    // A frame drawn faster than the steps come (above 50 frames a second) still gets the timer's call, with
    // no time in it - as a paused game's frames do. Without it the engine's per-frame work on the units is
    // skipped for that frame: vehicles' tracks and wheels flickered in both games.
    // No time means a speed of nought for the call: the timer gives a running clock at least one 5 ms tick
    // whatever the frame time, and those ticks made the battle run faster the faster the PC drew (5% at
    // 60 frames a second, half as fast again at 144) and in uneven steps (20, 20, 20, 20, 5 ms ...).
    // (2026 beta: a call at no speed also tells i_render, the timer's link CT_RENDER, that the game is paused, which
    //  puts its frame limiter on the paused rate; the link is taken away for this one call, which then tells nobody.)
    if (!steps) {
        float speed = *(float *)(ct + CT_MULT);
        void *render = CT_RENDER ? *(void **)(ct + CT_RENDER) : 0;
        if (CT_RENDER) *(void **)(ct + CT_RENDER) = 0;
        *(float *)(ct + CT_MULT) = 0;
        g_timer_step(ct, 0.0f, arg);
        *(float *)(ct + CT_MULT) = speed;
        if (CT_RENDER) *(void **)(ct + CT_RENDER) = render;
    }
}

// Every change of the selection - a click, a box, a hotkey - ends in adv_interf+0x57470(interface, list,
// ...): the list (unit pointers at +0, count at +0xc) becomes what is selected, and a selected unit's
// panel shows its state. In the stock game the box takes in whatever is under it, the computer's units
// too; against another player that would show his units' state, plan and ammunition. In a versus battle
// his units are therefore taken out of the list first: only this player's own units can be selected.
// (A unit's id - squad id plus its place in the squad - from its vtable slot +0x298.)
// 2026 beta: the routine is +0x611f0 (the same sixteen callers, select_group +0x5ef10 among them); it reads the
// unit's id from slot +0x2a0 (a method was inserted before it) and returns whether the list was the interface's
// own (its 4th argument 1) - what it returns is passed on.
#define UNIT_ID_SLOT BF(0x298, 0x2a0)
typedef u64 (*SetSelection)(void *, void *, u64, u64, u64, char);
static SetSelection g_set_selection;
static u64 g_stat_unselected;

static u64 set_selection_hook(void *interf, void *list, u64 a, u64 b, u64 c, char d)
{
    // MP_LOG_SELECT=1: every selection the interface makes, before and after the filter below
    static int log_sel = -1;
    if (log_sel < 0) { char v[8]; log_sel = GetEnvironmentVariableA("MP_LOG_SELECT", v, sizeof v) && atoi(v); }
    if (log_sel && readable(list, 0x10)) {
        void **items = *(void ***)list;
        u32 n = *(u32 *)((char *)list + 0xc);
        char names[160] = "";
        int at = 0;
        for (u32 i = 0; i < n && i < 4 && n < 4096 && readable(items, (size_t)n * 8) && at < 120; i++)
            if (readable(items[i], sizeof(Object))) at += sprintf_s(names + at, sizeof names - at, " %s", ((Object *)items[i])->full);
        mp_log("select: the interface selects %u units%s (a=%llu b=%llu c=%llu d=%d, from +0x%llx)", n, names, a, b, c, d,
               (u64)((char *)_ReturnAddress() - (char *)GetModuleHandleA("adv_interf.codelib")));     // (a log line only)
    }
    if (g_versus && readable(list, 0x10)) {
        void **items = *(void ***)list;
        u32 n = *(u32 *)((char *)list + 0xc), kept = 0;
        if (n && n < 4096 && readable(items, (size_t)n * 8)) {
            for (u32 i = 0; i < n; i++) {
                void *u = items[i];
                void **vt = readable(u, 8) ? *(void ***)u : 0;
                bool own = true;
                if (vt && readable(vt, UNIT_ID_SLOT + 8)) own = session_commands_squad(((u32 (*)(void *))vt[UNIT_ID_SLOT / 8])(u));
                if (own) items[kept++] = u;
            }
            if (kept != n) { g_stat_unselected += n - kept; *(u32 *)((char *)list + 0xc) = kept; }
            if (log_sel) mp_log("select: %u of them are this player's to command", kept);
        }
    }
    return g_set_selection(interf, list, a, b, c, d);
}

static void install_detours(void)
{
    static const unsigned char start_sig[] = { 0x40, 0x53, 0x48, 0x83, 0xec, 0x40, 0x48, 0x8b, 0xd9, 0x80, 0xb9, 0xda, 0x0e, 0x00, 0x00, 0x00 };
    static const unsigned char select_sig[] = { 0x48, 0x8b, 0xc4, 0x44, 0x89, 0x48, 0x20, 0x44, 0x88, 0x40, 0x18, 0x41, 0x55, 0x41, 0x56, 0x48, 0x83, 0xec, 0x58 };
    // the Start button's handler: in the public build one argument less is passed on (the key flag), so it begins otherwise
    static const unsigned char input_old[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x50, 0x49, 0x8b, 0xd8, 0x48, 0x8b, 0xf9 };
    static const unsigned char input_pub[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x40, 0x49, 0x8b, 0xf8, 0x48, 0x8b, 0xd9 };
    static const unsigned char box_sig[] = { 0x40, 0x55, 0x48, 0x83, 0xec, 0x30, 0x8b, 0x81, 0xe8, 0x0d, 0x00, 0x00, 0x48, 0x8b, 0xe9 };
    static const unsigned char timer_sig[] = { 0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x70, 0x10, 0x48, 0x89, 0x78, 0x20 };
    // 2026 beta (adv_interf +0x3e340, +0x3e480, +0x489b0, +0x611f0; base_main +0x8390): each signature is the
    // detour's length, all whole instructions - moves, pushes, a sub rsp and (start, box) a compare / load of
    // an interface field; none of them relative to rip.
    static const unsigned char start_b26[] = { 0x40, 0x53, 0x48, 0x83, 0xec, 0x40, 0x48, 0x8b, 0xd9, 0x80, 0xb9, 0x4a, 0x11, 0x00, 0x00, 0x00 };
    static const unsigned char input_b26[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xec, 0x50, 0x49, 0x8b, 0xd8, 0x48, 0x8b, 0xf1 };
    static const unsigned char box_b26[] = { 0x48, 0x89, 0x6c, 0x24, 0x18, 0x57, 0x48, 0x83, 0xec, 0x30, 0x8b, 0x81, 0x54, 0x10, 0x00, 0x00, 0x48, 0x8b, 0xe9 };
    static const unsigned char select_b26[] = { 0x44, 0x89, 0x4c, 0x24, 0x20, 0x44, 0x88, 0x44, 0x24, 0x18, 0x48, 0x89, 0x54, 0x24, 0x10, 0x53, 0x57, 0x41, 0x55, 0x48, 0x83, 0xec, 0x60 };
    static const unsigned char timer_b26[] = { 0x48, 0x8b, 0xc4, 0x44, 0x89, 0x40, 0x18, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57 };
    static bool done;
    if (done || !g_tactical || g_role == ROLE_NONE) return;
    if (!build_module("adv_interf.codelib") || !build_module("base_main.codelib")) return;
    done = true;
    const char *ai = "adv_interf.codelib";
    bool b26 = g_bi == BUILD_BETA26;
    const unsigned char *start_s = b26 ? start_b26 : start_sig, *input_s = BV(input_old, input_pub, input_b26), *box_s = b26 ? box_b26 : box_sig;
    const unsigned char *timer_s = b26 ? timer_b26 : timer_sig, *select_s = b26 ? select_b26 : select_sig;
    size_t start_n = b26 ? sizeof start_b26 : sizeof start_sig, input_n = b26 ? sizeof input_b26 : sizeof input_old, box_n = b26 ? sizeof box_b26 : sizeof box_sig;
    size_t timer_n = b26 ? sizeof timer_b26 : sizeof timer_sig, select_n = b26 ? sizeof select_b26 : sizeof select_sig;
    char *start = build_code(ai, BV(0x3b3b0, 0x3b320, 0x3e340), start_s, start_n);
    char *input = build_code(ai, BV(0x3b4f0, 0x3b460, 0x3e480), input_s, input_n);
    char *box = build_code(ai, BV(0x41700, 0x41630, 0x489b0), box_s, box_n);
    char *timer = build_code("base_main.codelib", BV(0x4200, 0x4200, 0x8390), timer_s, timer_n);
    char *select = build_code(ai, BV(0x57470, 0x57310, 0x611f0), select_s, select_n);
    // all three or none: a half-gated Start button would let one player skip the other
    if (start && input && box) {
        g_input_action = (InputAction)install_detour(input, input_n, input_action_hook);
        g_box_result = (BoxResult)install_detour(box, box_n, box_result_hook);
        g_start_action = (StartAction)install_detour(start, start_n, start_action_hook);
    }
    if (select) g_set_selection = (SetSelection)install_detour(select, select_n, set_selection_hook);
    else mp_log("net: unknown game build, the other player's units can be selected");
    char v[16];
    if (GetEnvironmentVariableA("MP_SIM_HZ", v, sizeof v)) g_sim_step = atoi(v) > 0 ? 1.0 / atoi(v) : 0;
    if (timer) g_timer_step = (TimerStep)install_detour(timer, timer_n, timer_step_hook);
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
    g_stepping_phase = true;        // (the pause it lifts on the way into the battle is the engine's, not a player's word)
    g_start_action(ui, 0, 0, 0);
    g_stepping_phase = false;
    g_my_pause = false;
}

// Tests: MP_TEST_MAXTIME=<seconds> shortens the battle. The engine ends a battle when its time is up; that
// time is the float "max_game_time" of the battle description, kept by i_gamespec at +0x304 (adv_game reads
// it there: lea rax,[rdi+0x304] next to the key "f:max_game_time"). Nothing else about the end changes.
// The limit is copied into a countdown when the battle is built (i_trigman +0x29c, seconds left, counted
// down while the battle runs; at zero the game shows its red end-of-battle clock and then ends the battle),
// so that is what is shortened once the battle is under way.
// (2026 beta: the same two fields - adv_game+0x181f10 reads "f:max_game_time" into i_gamespec +0x304, and
//  i_trigman's slot 21, +0x1724b0, returns +0x29c as beta16's +0xf16a0 does.)
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

// ---------------------------------------------------------------- joining game: back to the operational map
// A battle entered from the operational map (joiner_into_battle) goes back to that map program when it is over
// or the host has left it for his map: to look at, until the host's newest map replaces it (with orders again
// when no battle is left) - never to the menu. Sets the environment and writes the command line; false: the
// battle was not entered from the map.
static bool map_return_command(char *cmd, size_t size)
{
    char args[512], epoch[16] = "", peer[128] = "";
    if (!GetEnvironmentVariableA("MP_MAP_RETURN", args, sizeof args)) return false;
    GetEnvironmentVariableA("MP_MAP_RETURN_EPOCH", epoch, sizeof epoch);
    if (!GetEnvironmentVariableA("MP_MAP_RETURN_PEER", peer, sizeof peer)) GetEnvironmentVariableA("MP_AUTOJOIN", peer, sizeof peer);
    SetEnvironmentVariableA("MP_ROLE", "join");
    SetEnvironmentVariableA("MP_PEER", peer);
    SetEnvironmentVariableA("MP_MAP", "1");
    SetEnvironmentVariableA("MP_MAP_VIEW", "1");
    SetEnvironmentVariableA("MP_MAP_EPOCH", epoch);
    SetEnvironmentVariableA("MP_HOST_MODE", "operation");
    SetEnvironmentVariableA("MP_AUTOJOIN", 0);
    SetEnvironmentVariableA("MP_VERSUS", 0);
    if (!GetEnvironmentVariableA("MP_TEST", 0, 0)) SetEnvironmentVariableA("MP_WINDOW", 0);
    sprintf_s(cmd, size, "\"%s\\starter_x64.exe\" %s", g_root, args);
    mp_log("net: back to this player's operational map (%s)", args);
    return true;
}

// ---------------------------------------------------------------- joining game: the host is gone
// Without the host there is no battle to follow. A few seconds after the host closed its game or stopped
// answering, the joining game starts the menu program again (or, for a battle of an operation entered from
// the map, that map program) and closes this battle.
static void leave_battle_for_menu(void)
{
    static bool left;
    if (left) return;
    left = true;
    char cmd[MAX_PATH + 600];
    if (!map_return_command(cmd, sizeof cmd)) {
        mp_log("net: leaving the battle, back to the menu");
        SetEnvironmentVariableA("MP_ROLE", 0);
        SetEnvironmentVariableA("MP_PEER", 0);
        SetEnvironmentVariableA("MP_VERSUS", 0);
        if (!GetEnvironmentVariableA("MP_TEST", 0, 0)) SetEnvironmentVariableA("MP_WINDOW", 0);      // (tests: the menu stays on their screen)
        sprintf_s(cmd, "\"%s\\starter_x64.exe\"", g_root);
    }
    STARTUPINFOA si = { sizeof si };
    PROCESS_INFORMATION pi;
    if (CreateProcessA(0, cmd, 0, 0, FALSE, 0, 0, g_root, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    if (HWND w = main_window()) PostMessageA(w, WM_CLOSE, 0, 0);
}

// ---------------------------------------------------------------- joining game: the battle is over
// A battle that belongs to an operation ends by starting that operation's map program (base_shell is the
// only engine module that starts programs, through CreateProcessA). A joined game's battle belongs to the
// HOST's operation, which this player does not have: its way back is the menu, from where it finds the
// host again by itself. The call is caught in base_shell's import table.
typedef BOOL (WINAPI *CreateProcessFn)(LPCSTR, LPSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCSTR, LPSTARTUPINFOA, LPPROCESS_INFORMATION);
static CreateProcessFn g_create_process;

static BOOL WINAPI create_process_hook(LPCSTR app, LPSTR cmd, LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta, BOOL inherit, DWORD flags,
                                       LPVOID env, LPCSTR dir, LPSTARTUPINFOA si, LPPROCESS_INFORMATION pi)
{
    if (cmd && g_role == ROLE_JOIN && g_tactical && strstr(cmd, "opgame.progpack")) {
        // entered from the operational map: back to that map program (map_return_command) - never to the menu
        char back[MAX_PATH + 600];
        if (map_return_command(back, sizeof back)) {
            mp_log("net: the battle is over");
            return g_create_process(0, back, pa, ta, inherit, flags, env, g_root, si, pi);
        }
        mp_log("net: the battle is over - back to the menu, not to the host's operation (%s)", cmd);
        SetEnvironmentVariableA("MP_ROLE", 0);
        SetEnvironmentVariableA("MP_PEER", 0);
        SetEnvironmentVariableA("MP_VERSUS", 0);
        if (!GetEnvironmentVariableA("MP_TEST", 0, 0)) SetEnvironmentVariableA("MP_WINDOW", 0);      // (tests: the menu stays on their screen)
        char own[MAX_PATH + 8];
        sprintf_s(own, "\"%s\\starter_x64.exe\"", g_root);
        return g_create_process(0, own, pa, ta, inherit, flags, env, g_root, si, pi);
    }
    return g_create_process(app, cmd, pa, ta, inherit, flags, env, dir, si, pi);
}

static void catch_return_to_operation(void)
{
    static bool done;
    if (done || g_role != ROLE_JOIN || !g_tactical) return;
    done = true;
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
            g_create_process = (CreateProcessFn)slots->u1.Function;
            slots->u1.Function = (ULONGLONG)create_process_hook;
            VirtualProtect(&slots->u1.Function, 8, old, &old);
            mp_log(GetEnvironmentVariableA("MP_MAP_RETURN", 0, 0) ? "net: when this battle ends the game goes back to this player's operational map"
                                                                 : "net: when this battle ends the game goes back to the menu (and from there to the host)");
            return;
        }
    }
}

// ---------------------------------------------------------------- receive
static void on_packet(const sockaddr_in &from, const unsigned char *data, int size, DWORD now)
{
    const Head *h = (const Head *)data;
    if (size < (int)sizeof(Head) || h->magic != k_magic) return;
    bool from_peer = g_have_peer && same_addr(from, g_peer);

    if (g_role == ROLE_HOST && h->type == PKT_HELLO) {
        if (size < 24) return;
        const Hello *in = (const Hello *)data;
        if (memcmp(in->proto, k_protocol, sizeof k_protocol)) return;
        bool full = size >= (int)sizeof(Hello) && in->version == k_version;
        if (full && in->game_build != g_bi) {           // (the joining game sees Welcome.game_build and gives up itself)
            static DWORD said;
            if (now - said > 10000) { said = now; mp_log("net: a player with another game build (%d, this game %d) is turned away", in->game_build, g_bi); }
        }
        if (full) g_peer_in_lobby = in->phase == 0xff;
        unsigned prefer = full ? in->prefer_side : 0;
        // The joining player commands the opponent unless it asked for the side the host plays.
        bool versus = !(prefer && prefer == g_user_side);
        Welcome w = { make_head(PKT_WELCOME) };
        memcpy(w.proto, k_protocol, sizeof k_protocol);
        w.version = k_version;
        // (a battle that is over is not one to join: a game that comes back to the lobby from it waits there
        // until the host has left its statistics and is on the map or in the next battle)
        // (nor one that is about to start again with the joining player's own force)
        bool battle_on = g_tactical && !g_image.empty() && g_phase != 3 && !endgame_over() && !g_force_restarting &&
                         !(late_force_open() && !g_force_in.empty());
        w.image_size = battle_on ? (u32)g_image.size() : 0;
        if (battle_on && g_role == ROLE_HOST && g_battle_of[0]) {
            strcpy_s(w.battle_of, g_battle_of);
            w.extras_size = (u32)g_extras.size();
            w.extras_hash = g_extras_hash;
        }
        w.image_hash = g_image_hash;
        w.user_side = g_user_side;
        w.enemy_side = g_enemy_side;
        w.in_battle = battle_on;
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
        w.map_flags = (g_map_collect ? 1 : 0) | (g_turn_done ? 2 : 0) | (g_map_epoch << 8) |
                      (!g_tactical && g_host_mode == HOST_MODE_SKIRMISH && g_host_start_ready ? 4 : 0) |    // bit 2: the host is ready
                      (!g_tactical && g_map_given && g_map_view_shared ? 8 : 0);                          // bit 3: the copy is to look at
        w.game_build = g_bi;
        if (!g_tactical && g_host_mode == HOST_MODE_SKIRMISH) {
            w.force_have = g_peer_force_hash;
            w.force_refused = g_force_refused;
            w.force_side = session_planned_joiner_side();
            w.force_enemy = planned_host_side();
            w.force_limit = session_force_limit();
            setup_date(&w.force_year, &w.force_period);
        } else if (g_tactical && g_host_mode == HOST_MODE_SKIRMISH) {
            w.force_have = g_peer_force_hash;
            w.force_refused = g_force_refused;
            if (late_force_open()) {                    // (the host's battle, still in deployment)
                w.force_side = g_enemy_side;
                w.force_enemy = g_user_side;
                w.force_limit = session_force_limit();
                setup_date(&w.force_year, &w.force_period);
            }
        }
        send_to(from, &w, sizeof w);
        if (!full) return;                              // the caller reads the version from the reply and gives up
        char name[24];
        memcpy(name, in->name, sizeof name);
        name[sizeof name - 1] = 0;
        for (char *c = name; *c; c++) if (!isalnum((unsigned char)*c) && *c != '_' && *c != '-') *c = '_';
        if (!from_peer) {
            if (is_steam(from)) {
                // (the programs that follow this one - the battle, the map - take the same player on)
                char id[32];
                sprintf_s(id, "%llu", steam_id_of(from));
                SetEnvironmentVariableA("MP_STEAM_PEER", id);
                steam_accept(steam_id_of(from));
                mp_log("net: player connected through Steam (steam:%s, %s)", id, name[0] ? name : "no name");
            } else {
                char ip[64];
                inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
                SetEnvironmentVariableA("MP_STEAM_PEER", 0);
                mp_log("net: player connected from %s:%u (%s)", ip, ntohs(from.sin_port), name[0] ? name : "no name");
            }
            g_sent.clear();            // a new game needs every unit once
            g_held.clear();
            g_play.have = false;
            statesync_reset();
            shots_reset();
            anim_reset();
            g_peer_ready = 0;
            g_peer_swap_known = false;
            if (!g_tactical) peer_force_clear("another player");
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
        if (!g_tactical && g_host_mode == HOST_MODE_SKIRMISH && full) force_offer(in->force_size, in->force_hash, in->force_cost);
        else if (late_force_open() && full && in->force_size) force_offer(in->force_size, in->force_hash, in->force_cost);
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
            // flags 0: the battle description (host only); 1: the operational map this game is offering;
            // 2: the files of the operation this battle belongs to (host only)
            const std::vector<unsigned char> &blob = h->flags == 1 ? g_map_out : h->flags == 2 ? g_extras : g_image;
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
    case PKT_FORCE_DATA:
        if (g_role == ROLE_HOST && (!g_tactical || late_force_open()) && size >= (int)(sizeof(ImageData) - k_chunk)) force_piece((const ImageData *)data, size);
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
                if (g_map_view && w->map_size && epoch == g_map_base_epoch && !(w->map_flags & 8) && !w->in_battle) {
                    // this very state of the operation, now for orders: no battle is left to fight
                    g_map_view = false;
                    mp_log("map: the turn's battles are over - this player gives orders again");
                }
                if (w->in_battle) {
                    if (!g_map_battle_at) g_map_battle_at = now + 300;
                } else if (w->map_size && w->map_size <= (64u << 20) && epoch != g_map_base_epoch && w->map_hash != g_map_in_hash &&
                           !(w->map_flags & 1) && opsave::safe_file_name(w->map_file)) {
                    // the operation has moved on (a turn ended, a battle was fought): the host's newest map
                    g_map_in.assign(w->map_size, 0);
                    g_map_in_hash = w->map_hash;
                    g_map_have.assign((w->map_size + k_chunk - 1) / k_chunk, 0);
                    g_map_asked.assign(g_map_have.size(), 0);
                    g_map_got = 0;
                    g_map_in_epoch = epoch;
                    g_map_in_turns = w->map_turns;
                    g_map_in_view = (w->map_flags & 8) != 0;
                    memcpy(g_map_in_file, w->map_file, sizeof g_map_in_file);
                    g_map_in_file[sizeof g_map_in_file - 1] = 0;
                    mp_log("map: the host offers his newest map (%u bytes, %s)", w->map_size, g_map_in_view ? "to look at" : "for orders");
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
            {
                float speed = sh->want_speed >= 0.05f && sh->want_speed <= 16 ? sh->want_speed : 1;
                bool pause = sh->want_pause != 0;
                if (pause != g_peer_pause) mp_log("clock: the other player %s", pause ? "asks for a pause" : "no longer asks for a pause");
                if (speed != g_peer_speed) mp_log("clock: the other player asks for speed x%.2f", speed);
                g_peer_speed = speed;
                g_peer_pause = pause;
                if (sh->pad2[0]) { g_peer_fps = sh->pad2[0]; g_peer_fps_at = GetTickCount(); }
            }
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
            double sim = -1;
            if (sh->phase == 2 && g_phase == 2 && sh->sim_ms && !sh->paused) {
                sim = sh->sim_ms / 1000.0;
                double r = seconds_now(), speed = sh->speed > 0.05f && sh->speed <= 16 ? sh->speed : 1;
                Playout &p = g_play;
                double dt = r - p.at, resid = sim - play_clock(r);
                // (the rate the clock is kept near: the measured one once there is one. Kept near the reported
                //  speed alone, a clock running three times as fast as reported - MP_TEST_SPEED=3 - was followed
                //  at one and a half: the shown time fell behind, was reset half a second later, and every
                //  moving unit stood and jumped 2-3 m a few times a second)
                bool restart = !p.have || dt > 2.0;                     // first, a clock taken over, a pause
                // (a new speed: measured again - a change of a tenth or more, not the automatic slow-down's wobble)
                if (restart || fabs(speed - p.spd) > 0.1 * max(p.spd, 0.1)) { p.measured = 0; p.win_at = r; p.win_sim = sim; p.spd = speed; }
                // (measured over a second of transforms, the first time over a quarter; the window is not started
                //  again when the shown clock is put right - that happened every quarter second while the rate
                //  was wrong, and no window ever ended)
                bool rebase = false;
                if (!restart && sim > p.win_sim && r - p.win_at >= (p.measured > 0 ? 1.0 : 0.25)) {
                    double m = (sim - p.win_sim) / (r - p.win_at);
                    if (m > 0.05 && m < 40) p.measured = p.measured > 0 ? p.measured + 0.3 * (m - p.measured) : m;
                    p.win_at = r;
                    p.win_sim = sim;
                    // (a rate a fifth or more off what was measured: started again from here at once. Left to the
                    //  slow fit, the shown time ran away for most of a second at the start of every battle faster
                    //  than reported, the arrivals' wandering came out at 0.7 s and the delay at its 1.5 s top)
                    rebase = p.measured > 0 && fabs(p.rate - p.measured) > 0.2 * p.measured;
                }
                double toward = p.measured > 0 ? p.measured : speed;
                if (restart || rebase || fabs(resid) > 0.5 * max(toward, 1.0)) {
                    Playout was = p;
                    p = { true, sim, r, toward, 0.02, was.have ? was.interval : 0.066, sim, was.measured, was.win_at, was.win_sim, was.spd };
                } else if (sim > p.last) {
                    p.base = play_clock(r) + 0.1 * resid;
                    p.rate += 0.005 * resid / max(dt, 0.02);
                    p.rate = min(max(p.rate, 0.5 * min(toward, speed)), 1.5 * max(toward, speed));
                    p.at = r;
                    p.dev += 0.05 * (fabs(resid) - p.dev);
                    if (sim - p.last < 1.0) p.interval += 0.05 * ((sim - p.last) - p.interval);
                    p.last = sim;
                }
            }
            const UnitRec *recs = (const UnitRec *)(sh + 1);
            for (unsigned i = 0; i < sh->h.count; i++) g_pending.push_back({ recs[i], sim });
            g_stat_packets++;
        }
        break;
    case PKT_OBJECT:
    case PKT_OBJECT_ACK:
        statesync_on_packet(data, size, send_peer, g_versus);
        break;
    case PKT_ANIM:
        anim_on_packet(data, size);
        break;
    case PKT_VDMG:
        if (from_peer) vdamage_on_packet(data, size);
        break;
    case PKT_GUNS:
        if (from_peer) guns_on_packet(data, size);
        break;
    case PKT_SIDE:
        if (from_peer) endgame_on_packet(data, size);
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
    {
        static DWORD fps_from;
        static u32 fps_frames;
        fps_frames++;
        if (!fps_from) fps_from = now;
        else if (now - fps_from >= 1000) { g_my_fps = fps_frames * 1000.0f / (now - fps_from); fps_frames = 0; fps_from = now; }
    }
    update_ai_commander(om);
    catch_return_to_operation();
    if (g_tactical && (g_role != ROLE_NONE || GetEnvironmentVariableA("MP_LOG_SHOTS", 0, 0))) shots_install(om);
    if (g_tactical && g_role != ROLE_NONE) trench_install();
    if (g_tactical && (g_role != ROLE_NONE || GetEnvironmentVariableA("MP_LOG_ORDERS", 0, 0))) orders_install(om);
    if (g_tactical && g_role != ROLE_NONE) stats_install(om);
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
    // ... and what has come through Steam: for a host from anybody, for a joining game from its host
    steam_frame();
    if ((g_role == ROLE_HOST || (g_role == ROLE_JOIN && g_have_peer && is_steam(g_peer))) && steam_ready()) {
        for (int i = 0; i < 2048; i++) {
            u64 who = 0;
            int n = steam_recv(&who, buf, sizeof buf);
            if (n <= 0) break;
            g_stat_bytes += n;
            on_packet(steam_addr(who), buf, n, now);
            if (g_sock == INVALID_SOCKET) return;
        }
    }

    install_detours();
    install_map_detours();
    resign_if_asked(om);
    if (g_tactical && !g_interface) g_interface = engine_find(om, "i_intereff", "intereff");
    menu_tests(om);
    if (g_connected && g_tactical) shots_idle();
    endgame_frame(om, send_peer);
    host_map_frame(om, now);
    if ((g_role == ROLE_HOST && !g_tactical) || g_map_mode) request_map_pieces(now);
    if (g_role == ROLE_HOST && (!g_tactical || late_force_open())) request_force_pieces(now);
    if (g_map_restart_at && now >= g_map_restart_at) { g_map_restart_at = 0; host_merge_and_restart(om); }
    if (g_map_mode && g_collect_asked && !g_map_given && operation_on_screen(om)) { g_collect_asked = false; session_map_give(om); }
    if (g_map_mode && !g_scouted) {                          // the other player's map: his own scouting, once it is up
        static DWORD up_since;
        if (!operation_on_screen(om)) up_since = 0;
        else if (!up_since) up_since = now;
        else if (now - up_since > 2500) { g_scouted = true; joiner_scouting(om); }
    }
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
            hello.game_build = g_bi;
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
            anim_frame(send_peer);
            guns_frame(om, send_peer);
        }
        double t0 = seconds_now();
        if (sync_state_enabled()) statesync_owner_frame(om, now, owns_object, send_peer);
        g_prof_save += seconds_now() - t0;
        xlate_frame(om);
        stats_frame();
        shots_frame(send_peer);
        vdamage_frame(om, send_peer);
    }

    if (g_connected && now - g_last_rx > 8000) {
        mp_log("net: connection lost (nothing received for 8 s)");
        g_connected = false;
        g_peer_pause = false;
        g_peer_speed = 1;
        g_peer_ready = 0;
        if (g_role == ROLE_JOIN && g_tactical && !g_host_lost) g_host_lost = now ? now : 1;
        if (g_role == ROLE_HOST) { g_have_peer = false; g_peer_on_map = g_peer_turn_done = false; }
    }
    // (a battle that is over is left by its player, from the statistics, when he has read them)
    if (g_host_lost && now - g_host_lost > 4000 && !endgame_over() && g_phase != 3) leave_battle_for_menu();
    if (now - g_last_report >= 5000) {
        if (g_connected && g_tactical && (g_stat_sent || g_stat_applied)) {
            mp_log("net: 5 s: sent %u and applied %u unit transforms (%u times a standing vehicle left to this game's physics), mean correction %.2f m, largest %.2f m, %u jumps, %.1f KB/s in total",
                   g_stat_sent, g_stat_applied, g_stat_rest, g_stat_applied ? g_stat_err_sum / g_stat_applied : 0.0, g_stat_err_max, g_stat_snaps, g_stat_bytes / 5120.0);
            if (g_play.have && g_phase == 2)
                mp_log("net: the other game's units shown by its battle clock: transforms %.0f ms apart, its clock %+.0f ms from this game's and running x%.3f, arrivals wandering %.0f ms, shown %.0f ms behind",
                       g_play.interval * 1000, (play_clock(seconds_now()) - g_sim_ms / 1000.0) * 1000, g_play.rate, g_play.dev * 1000, play_delay() * 1000);
            statesync_report();
            if (g_stat_unselected) { mp_log("select: 5 s: %llu of the other player's units kept out of this player's selection", g_stat_unselected); g_stat_unselected = 0; }
            shots_report();
            trench_report();
            anim_report();
            hits_report();
            orders_report();
            vdamage_report();
            guns_report();
            fires_report();
            stats_report();
            steam_report();
        if (g_stat_led || g_stat_not_led) {
            mp_log("net: AI commander: %llu steps for formations of this side, %llu skipped for the other player's", g_stat_led, g_stat_not_led);
            g_stat_led = g_stat_not_led = 0;
        }
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
        report_jumps();
        g_stat_sent = g_stat_applied = g_stat_packets = g_stat_bytes = g_stat_snaps = g_stat_rest = 0;
        g_stat_err_sum = 0;
        g_stat_err_max = 0;
    }
}
