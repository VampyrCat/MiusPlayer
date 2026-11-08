// The end of a battle: a player withdrawing, a ceasefire, and the moment the battle is over.
//
// All of it is the trigger manager's (i_trigman, adv_game, vtable +0x130640). Every five seconds of battle
// (+0xef380) it looks at the two sides and through its own virtual slots
//     slot 16 (+0x80, +0xef8a0)  flee(side)            the side leaves the battle: a flag on the side, the
//                                                      warning "the enemy is withdrawing" and the end-of-battle
//                                                      countdown (+0x288) set to the retreat time (+0x28c);
//                                                      also what the Withdraw button of the ESC menu calls
//                                                      (message box 0, "it_mb_flee_ok")
//     slot 13 (+0x68, +0xeeb00)  truce(side, want)     the computer's side asks for a ceasefire / fights on
//     slot 14 (+0x70, +0xeef10)  truce(want)           the player does (the game's own "accept ceasefire" box)
// keeps +0x294, one bit per side that wants a ceasefire; with both set the countdown runs for 30 s
// ("all sides want truce"). When the countdown or the battle's time (+0x29c) runs out, +0x103ef0 works out
// the result from the state of the two forces and ends the battle through +0xef670 (result 0..6: 3 a draw,
// above it a victory of the player's side), which broadcasts "end_game" and brings up the statistics.
//
// Each game has its own trigger manager and none of this crossed the wire: a player who withdrew got his
// end of battle and the other player never heard of it; each game ended its battle when its own countdown
// said so, with its own verdict. Now:
//   - A side leaving the battle is an event of the game whose player commands that side (versus), or of the
//     host for the computer's side: carried out there, sent, and carried out by the same routine in the
//     other game - which shows the engine's own "enemy is withdrawing" warning and starts the same countdown.
//     What a game's trigger manager decides by itself about the OTHER player's side is dropped.
//   - A ceasefire is each player's own word. The ESC menu has a CEASEFIRE button (ui.cpp): a press sets this
//     player's bit here and, in the other game, the bit of the side he commands there - by the engine's own
//     routines, so the other player gets the game's "the enemy proposes a ceasefire" warning. A second press
//     takes the word back. Once both have given it the engine's countdown runs, shortened to five seconds,
//     and the battle ends. The computer never speaks for a side a player commands.
//   - The battle is over when the HOST's game says so. A joined game whose own countdown runs out waits; when
//     the host's result arrives it ends its battle in that step, from its own point of view, with what the
//     host knew about who left and which trigger ended it. Its verdict is the host's seen from the other
//     side (6 - result in a versus game); should its own numbers have given another, the host's stands.
//   - What is left of a force is what the game of the player who commands it says. The trigger manager adds
//     each side up from its squads (+0x104b20: four numbers per side at the start and now - force, ground
//     held ... - kept by i_gamespec), and a game's picture of the OTHER player's squads is not exact: after a
//     minute of an operation battle the host counted 16 points of Soviet losses where the Soviet player's
//     own game had 4, and the two end screens showed different tables. In a versus game each game therefore
//     sends its own side's numbers a few times a second (PKT_SIDE) and uses the other game's count of the
//     other side's force whenever the engine adds the sides up. The host's last word carries the numbers its result was made
//     from; the joined game works its own result out from exactly those, so the two end screens show one
//     table from two sides.
// Records travel on the shots' reliable, ordered channel (ShotRec.kind 8, .flags says which).
// MP_SYNC_END=0 switches all of it off (each game ends its own battle, as before).
//
// beta26 (public_beta-26494568; read in IDA): vtable +0x1c3628 with 48 slots (one added at the end), slots 13 / 14 /
// 16 +0x16fa80 / +0x16fe90 / +0x170820, evaluate +0x1878b0, end +0x1705f0, side sums +0x1885e0, the same code. The
// object is 0xe00 bytes: +0xf8..+0x2a8 stayed, the timer +0x2ac moved to +0xc54 and the evaluation block +0x2b0..+0x372
// to +0xc60..+0xd22 (+0x9b0, and +0x43c..+0x442 with it); the score tables are +0x204a08 / +0x204a38. New there: the
// evaluation lowers the result of a battle a side left to a minor defeat of that side - only the host's game
// evaluates (a joined game takes the host's result), so it changes nothing here.
#include "common.h"
#include "build.h"
#include "detour.h"
#include "protocol.h"
#include <stdlib.h>
#include <string.h>
#include <vector>

enum { TM_SLOTS_MAX = 48, TM_AI_TRUCE = 13, TM_USER_TRUCE = 14, TM_FLEE = 16 };
#define TM_SLOTS BF(47, 48)
// trigger manager fields: link to i_gamespec, end countdown, retreat time, truce bits (one per side), battle time
// left, the result the end function stores, the enemy's truce timer, the evaluation's score factors, level, result,
// scores, "the player's side won", the evaluation's guard, the side and flag of the trigger that ended the battle,
// "the countdown is the truce's"; TM_SIZE what is read of the object
#define TM_GAMESPEC        0xf8
#define TM_COUNTDOWN       0x288
#define TM_RETREAT_TIME    0x28c
#define TM_TRUCE_MASK      0x294
#define TM_TIME_LEFT       0x29c
#define TM_END_MODE        0x2a8
#define TM_COOLDOWN        BF(0x2ac, 0xc54)
#define TM_OWN_K           BF(0x30c, 0xcbc)
#define TM_ENEMY_K         BF(0x310, 0xcc0)
#define TM_LEVEL           BF(0x320, 0xcd0)
#define TM_RESULT          BF(0x324, 0xcd4)
#define TM_SCORE_OWN       BF(0x32c, 0xcdc)
#define TM_SCORE_ENEMY     BF(0x330, 0xce0)
#define TM_WON             BF(0x370, 0xd20)
#define TM_DONE            BF(0x372, 0xd22)
#define TM_TRIG_SIDE       BF(0x43c, 0xdec)
#define TM_TRIG_FLAG       BF(0x440, 0xdf0)
#define TM_TRUCE_COUNTDOWN BF(0x442, 0xdf2)
#define TM_SIZE            BF(0x448, 0xdf8)
// the two sides as i_gamespec keeps them (vtable slot 5): two records of 0x30 bytes - the player's side
// first - with flags at +0x20 (bit 25: has left the battle), then the two side codes (the same in beta26: its flee,
// truce and end check read them so)
enum { SD_BEGIN = 0, SD_END = 0x10, SD_FLAGS = 0x20, SD_REC = 0x30, SD_USER = 0x90, SD_ENEMY = 0x94, SD_FLED = 0x2000000 };
enum { EV_FLEE = 1, EV_TRUCE = 2, EV_END = 3, EV_WISH = 4 };
static const float k_agreed_seconds = 5;       // from both players' word to the end of the battle

typedef void (*Flee)(Object *tm, u32 side);
typedef void (*AiTruce)(Object *tm, u32 side, char want);
typedef void (*UserTruce)(Object *tm, char want);
typedef void (*EndBattle)(Object *tm, u64 a, u64 b);
typedef void (*EndPhase)(Object *tm, u32 result, u64 c, u64 d);
typedef void (*Recalc)(Object *tm, float dt, u64 force, u64 mode);

static Object *g_tm;
static Flee g_flee;
static AiTruce g_ai_truce;
static UserTruce g_user_truce;
static EndBattle g_end_battle;
static EndPhase g_end_phase;
static Recalc g_recalc;
static float g_peer_end[4];                    // the other player's side as his game counts it
static bool g_have_peer_side;
static float g_final[2][2][4];                 // joined game, battle over: both sides (this player's first), start and end, from the host
static bool g_have_final;
static DWORD g_last_side;
static bool g_enabled = true, g_tried, g_installed, g_applying, g_manual;
static bool g_my_wish, g_peer_wish;            // the two players' word for a ceasefire
static bool g_over, g_host_over, g_waiting_logged;
static int g_host_result = -1;                 // joined game: the host's verdict (as the host sees it)
static bool g_host_versus;
static std::vector<ShotRec> g_wait;
static u32 g_stat_dropped;

static bool active(void)
{
    return g_installed && session_role() != ROLE_NONE && session_connected() && session_versus_or_coop();
}

static char *sides(Object *tm)
{
    Object *gs = *(Object **)((char *)tm + TM_GAMESPEC);
    if (!gs || !readable(gs, sizeof(Object))) return 0;
    char *s = ((char *(*)(Object *))gs->vtbl[5])(gs);
    return readable(s, 0x98) ? s : 0;
}

static u32 side_code(Object *tm, int which)
{
    char *s = sides(tm);
    return s ? *(u32 *)(s + (which ? SD_ENEMY : SD_USER)) : 0;
}

static int side_index(Object *tm, u32 side)
{
    return side && side == side_code(tm, 0) ? 0 : side && side == side_code(tm, 1) ? 1 : -1;
}

static bool fled(Object *tm, int index)
{
    char *s = sides(tm);
    return s && index >= 0 && (*(u32 *)(s + index * SD_REC + SD_FLAGS) & SD_FLED);
}

static void set_fled(Object *tm, int index, bool yes)
{
    char *s = sides(tm);
    if (!s) return;
    u32 *flags = (u32 *)(s + index * SD_REC + SD_FLAGS);
    *flags = yes ? *flags | SD_FLED : *flags & ~(u32)SD_FLED;
}

static const char *side_name(u32 side)
{
    static char name[2][8];
    static int at;
    char *n = name[at ^= 1];
    memcpy(n, &side, 4);
    n[4] = 0;
    return n;
}

static float &countdown(Object *tm) { return *(float *)((char *)tm + TM_COUNTDOWN); }
static u32 mask(Object *tm) { return *(u32 *)((char *)tm + TM_TRUCE_MASK); }

static void send_event(int what, u32 side, u32 value, float seconds = 0, u32 bits = 0, u32 extra = 0)
{
    ShotRec s = {};
    s.kind = 8;
    s.flags = (unsigned char)what;
    s.ammo = side;
    s.weapon = value;
    s.arg6 = bits;
    s.ammo_code = extra;
    s.ammo_check[0] = seconds;
    shots_queue(s);
}

// Both players have given their word: the engine's 30 seconds become five.
static void shorten_agreed(Object *tm)
{
    if (!*((char *)tm + TM_TRUCE_COUNTDOWN) || !(g_my_wish && g_peer_wish)) return;
    if (countdown(tm) > k_agreed_seconds) {
        countdown(tm) = k_agreed_seconds;
        mp_log("end: both players want the ceasefire - the battle ends in %g s", k_agreed_seconds);
    }
}

// Same side (no versus): the players' side asks for the ceasefire once both players do, and then gets it.
static void coop_apply(Object *tm)
{
    bool both = g_my_wish && g_peer_wish;
    g_applying = true;
    if (both && (mask(tm) & 3) != 3) {
        *(float *)((char *)tm + TM_COOLDOWN) = 0;
        g_user_truce(tm, 1);
        g_ai_truce(tm, side_code(tm, 1), 1);
    } else if (!both && (mask(tm) & 1)) {
        g_user_truce(tm, 0);
    }
    g_applying = false;
    shorten_agreed(tm);
}

// After the engine has added the two sides up: the other player's side is what his game says it is.
static void recalc_hook(Object *tm, float dt, u64 force, u64 mode)
{
    g_recalc(tm, dt, force, mode);
    if (!g_installed || tm != g_tm) return;
    char *s = sides(tm);
    if (!s) return;
    if (g_have_final) {
        for (int i = 0; i < 2; i++) {
            memcpy(s + i * SD_REC + SD_BEGIN, g_final[i][0], 16);
            memcpy(s + i * SD_REC + SD_END, g_final[i][1], 16);
        }
    } else if (g_have_peer_side && active() && session_versus()) {
        // the first of the four numbers: the force, added up from the side's squads. (The second, the ground
        // held, comes from who owns each square of the map - the same in both games - and a side that
        // leaves the battle loses it here, in the last adding-up, which only the game that ends the battle
        // has done.)
        memcpy(s + SD_REC + SD_END, g_peer_end, 4);
    }
}

void endgame_on_packet(const unsigned char *data, int size)
{
    if (size < (int)sizeof(SidePacket) || !g_installed) return;
    const SidePacket *p = (const SidePacket *)data;
    if (p->side != side_code(g_tm, 1)) return;
    memcpy(g_peer_end, p->end, sizeof g_peer_end);
    g_have_peer_side = true;
}

// The host's last word: the battle is over, with what its result was made from.
static void send_end(Object *tm)
{
    ShotRec s = {};
    s.kind = 8;
    s.flags = EV_END;
    s.weapon = *(u32 *)((char *)tm + TM_END_MODE);
    s.arg6 = (fled(tm, 0) ? 1 : 0) | (fled(tm, 1) ? 2 : 0) | (session_versus() ? 4 : 0) | (*((char *)tm + TM_TRIG_FLAG) ? 8 : 0);
    s.ammo_code = *(u32 *)((char *)tm + TM_TRIG_SIDE);
    if (char *sd = sides(tm)) {
        for (int i = 0; i < 2; i++) {
            memcpy(s.launch + i * 32, sd + i * SD_REC + SD_BEGIN, 16);
            memcpy(s.launch + i * 32 + 16, sd + i * SD_REC + SD_END, 16);
        }
        s.has_pos = 1;
    }
    shots_queue(s);
}

static void flee_hook(Object *tm, u32 side)
{
    if (g_applying || !active()) { g_flee(tm, side); return; }
    int index = side_index(tm, side);
    // versus: a player's side is his game's to give up; otherwise the host decides, and a player's own
    // press of Withdraw counts wherever it is made
    bool allowed = session_versus() ? index == 0 : (g_manual || session_role() == ROLE_HOST);
    if (!allowed || index < 0) {
        if (!g_stat_dropped++) mp_log("end: this game's own view that the side %s should leave the battle is not acted on - that is for the game that commands it", side_name(side));
        return;
    }
    bool was = fled(tm, index);
    g_flee(tm, side);
    if (was || !fled(tm, index)) return;
    mp_log("end: the side %s leaves the battle%s - the battle ends in %.0f s; the other game is told", side_name(side),
           g_manual ? " (Withdraw pressed)" : "", countdown(tm));
    send_event(EV_FLEE, side, g_manual, countdown(tm));
}

static void ai_truce_hook(Object *tm, u32 side, char want)
{
    if (g_applying || !active()) { g_ai_truce(tm, side, want); return; }
    if (session_versus() || session_role() != ROLE_HOST) return;        // a player speaks for that side, or the host's game does
    u32 before = mask(tm);
    g_ai_truce(tm, side, want);
    if (mask(tm) != before) send_event(EV_TRUCE, side, want != 0);
}

static void user_truce_hook(Object *tm, char want)
{
    if (g_applying || !active()) { g_user_truce(tm, want); return; }
    g_my_wish = want != 0;
    mp_log("end: this player %s", want ? "proposes a ceasefire" : "takes the ceasefire proposal back");
    if (session_versus()) {
        g_user_truce(tm, want);
        send_event(EV_TRUCE, side_code(tm, 0), want != 0);
        shorten_agreed(tm);
    } else {
        send_event(EV_WISH, 0, want != 0);
        coop_apply(tm);
    }
}

static void end_battle_hook(Object *tm, u64 a, u64 b)
{
    if (!active() || *((char *)tm + TM_DONE)) { g_end_battle(tm, a, b); return; }
    if (session_role() == ROLE_JOIN) {
        // (the countdown has been put back by the caller: nothing else to undo)
        if (!g_waiting_logged) { g_waiting_logged = true; mp_log("end: the battle has run out here - it ends when the host's game ends it"); }
        return;
    }
    g_end_battle(tm, a, b);
    if (!*((char *)tm + TM_DONE)) return;
    g_over = true;
    u32 result = *(u32 *)((char *)tm + TM_END_MODE);
    u32 bits = (fled(tm, 0) ? 1 : 0) | (fled(tm, 1) ? 2 : 0) | (session_versus() ? 4 : 0) | (*((char *)tm + TM_TRIG_FLAG) ? 8 : 0);
    mp_log("end: the battle is over here (result %u%s%s) - the other game is told to end its battle now", result,
           bits & 1 ? ", this side left" : "", bits & 2 ? ", the other side left" : "");
    cells_flush();              // (the ground as it stands at the end goes first)
    send_end(tm);
}

// The verdict goes through here on its way to the statistics. A joined game shows the host's.
static void end_phase_hook(Object *tm, u32 result, u64 c, u64 d)
{
    if (g_host_result >= 0 && session_role() == ROLE_JOIN) {
        u32 want = g_host_versus ? 6 - (u32)g_host_result : (u32)g_host_result;
        if (want <= 6 && want != result) {
            mp_log("end: this game's own numbers gave result %u, the host's give %u - the host's stand", result, want);
            char *t = (char *)tm, *base = build_module("adv_game.codelib");
            *(u32 *)(t + TM_RESULT) = want;
            *(u32 *)(t + TM_LEVEL) = want > 3 ? want - 3 : 3 - want;
            *(t + TM_WON) = want > 3;
            // the two scores as +0x103ef0 (beta26 +0x1878b0) makes them: a table entry by result, times the side's
            // factor (the tables are filled by the first evaluation: zero on disk)
            const u32 *own = base ? (const u32 *)(base + BV(0x15ca40, 0x15ca40, 0x20aa58)) : 0;
            const u32 *enemy = base ? (const u32 *)(base + BV(0x15ca70, 0x15ca70, 0x20aa88)) : 0;
            if (readable(own, 28) && readable(enemy, 28)) {
                *(float *)(t + TM_SCORE_OWN) = (float)own[want] * *(float *)(t + TM_OWN_K);
                *(float *)(t + TM_SCORE_ENEMY) = (float)enemy[want] * *(float *)(t + TM_ENEMY_K);
            }
            result = want;
        } else {
            mp_log("end: result %u here - the host's verdict%s", result, g_host_versus ? " seen from this side" : "");
        }
    }
    g_over = true;
    g_end_phase(tm, result, c, d);
}

static void apply(ObjMan *om, const ShotRec &s)
{
    Object *tm = g_tm;
    char *t = (char *)tm;
    if (s.flags == EV_FLEE) {
        int index = side_index(tm, s.ammo);
        if (index < 0 || fled(tm, index)) return;
        g_applying = true;
        g_flee(tm, s.ammo);
        g_applying = false;
        if (s.ammo_check[0] > 0) countdown(tm) = s.ammo_check[0];
        mp_log("end: the side %s leaves the battle%s, says its game - carried out here; the battle ends in %.0f s", side_name(s.ammo),
               s.weapon ? " (its player pressed Withdraw)" : "", countdown(tm));
    } else if (s.flags == EV_TRUCE) {
        // the other player's word (versus), or the host's computer opponent's
        if (session_versus()) g_peer_wish = s.weapon != 0;
        *(float *)(t + TM_COOLDOWN) = 0;        // (the engine listens to the computer's side once in five minutes)
        g_applying = true;
        g_ai_truce(tm, s.ammo, s.weapon != 0);
        g_applying = false;
        mp_log("end: the side %s %s, says its game", side_name(s.ammo), s.weapon ? "proposes a ceasefire" : "fights on");
        shorten_agreed(tm);
    } else if (s.flags == EV_WISH) {
        g_peer_wish = s.weapon != 0;
        mp_log("end: the other player %s", s.weapon ? "proposes a ceasefire" : "takes the ceasefire proposal back");
        if (!session_versus()) coop_apply(tm);
    } else if (s.flags == EV_END && session_role() == ROLE_JOIN) {
        if (g_host_over || *(t + TM_DONE) || session_phase() != 2) return;
        g_host_over = true;
        g_host_result = (int)s.weapon;
        g_host_versus = (s.arg6 & 4) != 0;
        // what the host knew: who left the battle (its own side is the other one here in a versus game),
        // and the trigger that ended it
        set_fled(tm, 0, (s.arg6 & (g_host_versus ? 2 : 1)) != 0);
        set_fled(tm, 1, (s.arg6 & (g_host_versus ? 1 : 2)) != 0);
        *(u32 *)(t + TM_TRIG_SIDE) = s.ammo_code;
        *(t + TM_TRIG_FLAG) = (s.arg6 & 8) != 0;
        if (s.has_pos) {
            // the host's two sides, its own first: in a versus game this player's is its second
            for (int i = 0; i < 2; i++) memcpy(g_final[i], s.launch + (g_host_versus ? 1 - i : i) * 32, 32);
            g_have_final = true;
        }
        countdown(tm) = -1;
        *(t + TM_TRUCE_COUNTDOWN) = 0;
        mp_log("end: the host's battle is over (result %u there) - ending this one", s.weapon);
        session_game_menu(om, false);           // the statistics come up over the battle, not over the ESC menu
        cells_flush();                          // the ground the host's result was made from
        g_end_battle(tm, 0, 0);
    }
}

void endgame_received(const ShotRec &s)
{
    if (g_wait.size() < 64) g_wait.push_back(s);
}

// Marks the Withdraw button's own call (session.cpp, around the game's message-box handler).
void endgame_manual(bool on) { g_manual = on; }
bool endgame_over(void) { return g_over; }

static void install(ObjMan *om)
{
    // (whole instructions, none position dependent: the detours copy all of each)
    static const unsigned char end_sig[] = { 0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8b, 0xec };
    // beta26 +0x1878b0: mov rax,rsp; mov [rax+20h],rbx; push rbp/rsi/rdi/r12/r13/r14/r15; lea rbp,[rax-5fh]
    static const unsigned char end_sig26[] = { 0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x20, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56,
                                               0x41, 0x57, 0x48, 0x8d, 0x68, 0xa1 };
    // (the same first bytes in beta26: end +0x1705f0, side sums +0x1885e0)
    static const unsigned char phase_sig[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57 };
    static const unsigned char recalc_sig[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57 };
    Object *tm = engine_find(om, "i_trigman", "trigman");
    const char *ag = "adv_game.codelib";
    char *base = build_module(ag);
    if (!tm || !base) return;
    g_tried = true;
    char v[16];
    if (GetEnvironmentVariableA("MP_SYNC_END", v, sizeof v) && !atoi(v)) { g_enabled = false; mp_log("end: each game ends its own battle (MP_SYNC_END=0)"); return; }
    void **vt = tm->vtbl;
    const unsigned char *esig = BV(end_sig, end_sig, end_sig26);
    size_t esig_n = BV(sizeof end_sig, sizeof end_sig, sizeof end_sig26);
    char *evaluate = build_code(ag, BV(0x103ef0, 0x103ef0, 0x18d2c0), esig, esig_n);
    char *end = build_code(ag, BV(0xef670, 0xef670, 0x176000), phase_sig, sizeof phase_sig);
    if (!readable(tm, TM_SIZE) || (char *)vt != base + BV(0x130640, 0x130640, 0x1c9040) ||
        (char *)vt[TM_AI_TRUCE] != base + BV(0xeeb00, 0xeeb00, 0x175490) || (char *)vt[TM_USER_TRUCE] != base + BV(0xeef10, 0xeef10, 0x1758a0) ||
        (char *)vt[TM_FLEE] != base + BV(0xef8a0, 0xef8a0, 0x176230) || !evaluate || !end || !sides(tm)) {
        mp_log("end: unknown game build, each game ends its own battle");
        return;
    }
    g_end_battle = (EndBattle)install_detour(evaluate, esig_n, end_battle_hook);
    g_end_phase = (EndPhase)install_detour(end, sizeof phase_sig, end_phase_hook);
    if (!g_end_battle || !g_end_phase) { mp_log("end: could not be hooked, each game ends its own battle"); return; }
    if (char *sums = build_code(ag, BV(0x104b20, 0x104b20, 0x18dff0), recalc_sig, sizeof recalc_sig))
        g_recalc = (Recalc)install_detour(sums, sizeof recalc_sig, recalc_hook);
    // a private copy of the class's function table for this one object (with the type information pointer
    // that sits just before it): the trigger manager and the interface both call these through it
    static void *copy[TM_SLOTS_MAX + 1];
    memcpy(copy, vt - 1, (TM_SLOTS + 1) * sizeof(void *));
    g_flee = (Flee)vt[TM_FLEE];
    g_ai_truce = (AiTruce)vt[TM_AI_TRUCE];
    g_user_truce = (UserTruce)vt[TM_USER_TRUCE];
    copy[1 + TM_FLEE] = flee_hook;
    copy[1 + TM_AI_TRUCE] = ai_truce_hook;
    copy[1 + TM_USER_TRUCE] = user_truce_hook;
    tm->vtbl = copy + 1;
    g_tm = tm;
    g_installed = true;
    mp_log("end: withdrawing, a ceasefire and the end of the battle are shared between the two games (retreat time %.0f s, %.0f s of battle left)",
           *(float *)((char *)tm + TM_RETREAT_TIME), *(float *)((char *)tm + TM_TIME_LEFT));
}

// Every frame, from the logic pass.
void endgame_frame(ObjMan *om, void (*send)(const void *, int))
{
    if (!g_tried && g_enabled && session_role() != ROLE_NONE && !strcmp(g_program, "tacgame")) install(om);
    if (!g_installed) { g_wait.clear(); return; }
    // this player's side as this game counts it: twice a second, ten times when the battle is about to end
    DWORD now = GetTickCount();
    float left = countdown(g_tm);
    if (g_recalc && active() && session_versus() && !g_over && session_phase() == 2 && now - g_last_side >= (left >= 0 && left < 8 ? 100u : 500u)) {
        g_last_side = now;
        if (char *s = sides(g_tm)) {
            SidePacket p = { make_head(PKT_SIDE) };
            p.side = side_code(g_tm, 0);
            memcpy(p.begin, s + SD_BEGIN, 16);
            memcpy(p.end, s + SD_END, 16);
            send(&p, sizeof p);
        }
    }
    if (g_wait.empty()) return;
    std::vector<ShotRec> events;
    events.swap(g_wait);
    for (const ShotRec &s : events) apply(om, s);
}

// A new connection: where this game stands, once more.
void endgame_resend(void)
{
    g_peer_wish = false;
    if (!g_installed) return;
    Object *tm = g_tm;
    bool versus = session_versus(), host = session_role() == ROLE_HOST;
    for (int index = 0; index < 2; index++)
        if (fled(tm, index) && (versus ? index == 0 : host)) send_event(EV_FLEE, side_code(tm, index), 0, countdown(tm));
    if (g_my_wish) send_event(versus ? EV_TRUCE : EV_WISH, versus ? side_code(tm, 0) : 0, 1);
    if (!versus && host && (mask(tm) & 2)) send_event(EV_TRUCE, side_code(tm, 1), 1);
    if (host && g_over) send_end(tm);
}

// For the CEASEFIRE button: 0 = no ceasefire to be had now; otherwise 4, plus 1 when this player has given
// his word and 2 when the other player has.
int endgame_truce(void)
{
    if (!active() || g_over || session_phase() != 2 || *((char *)g_tm + TM_DONE)) return 0;
    if (fled(g_tm, 0) || fled(g_tm, 1)) return 0;
    return 4 | (g_my_wish ? 1 : 0) | (g_peer_wish ? 2 : 0);
}

// Seconds until the battle ends, below zero when nothing is counting.
float endgame_countdown(void)
{
    return g_installed && !g_over ? countdown(g_tm) : -1;
}

// 1: this player's side is leaving the battle, 2: the other side is.
int endgame_leaving(void)
{
    if (!g_installed || g_over) return 0;
    return (fled(g_tm, 0) ? 1 : 0) | (fled(g_tm, 1) ? 2 : 0);
}

// The CEASEFIRE button (from the logic pass): this player's word, given or taken back - through the
// trigger manager's own slot, like the game's "accept ceasefire" box.
void endgame_toggle_truce(void)
{
    if (!endgame_truce()) return;
    ((UserTruce)g_tm->vtbl[TM_USER_TRUCE])(g_tm, !g_my_wish);
}
