// Who holds the ground (mpmod2): the map's control cells are the host's.
//
// The trigger manager (i_trigman) divides the map into cells (+0x240, records of 0x100 bytes, count +0x24c), each with
// the side that holds it (+0x50, a four-letter side code; +0x54 the owner this player is shown). Every 5.31 s the game
// adds the cells up into zones and into each side's ground in i_gamespec - what the end of the battle scores as
// "control" and, in an operation, who holds the battlefield. A cell changes hands only through one routine, the
// capture (beta16 / public adv_game +0xf4ff0, beta26 +0x176640 - read in IDA in all three builds; the Mius Framework
// research, kb/engine/battle-phases.md section 8):
//     capture(trigman, cell, side, show): nothing if the side has broken (i_gamespec side flags 0x2000000); else the
//     change counter +0x270 goes up, "key point taken" for this player's side, a reward to the capturing unit (+0xf8,
//     cleared), the owner set, the shown owner set when this player can see it, the cell's own capture (timer +0xe8,
//     side +0xf0) stopped.
// Each game called it from its own scan of the platoons in every cell, on its own timing, so the two games' ground
// drifted apart and the end screens' control did too. Now the host's game sends every change of owner (ShotRec kind
// 17: arg6 the first cell, ammo how many, launch their owners), all of them again on a new connection; the joining
// game makes no capture of its own and makes each of the host's through the same routine - with its warnings and its
// fog of war - without the reward (that unit is the host's). When the host's battle ends, its last changes go before
// its end, and the joining game takes them before it works out its result. MP_SYNC_CELLS=0 switches this off.
#include "common.h"
#include "build.h"
#include "detour.h"
#include "protocol.h"
#include <string.h>

#define TM_VTABLE   BV(0x130640, 0x130640, 0x1c9040)
#define CAPTURE_FN  BV(0xf4ff0, 0xf4ff0, 0x17c050)
enum {
    TM_CELLS = 0x240, TM_CELL_N = 0x24c, TM_CHANGES = 0x270,
    CELL_REC = 0x100, CELL_OWNER = 0x50, CELL_SHOWN = 0x54, CELL_TIMER = 0xe8, CELL_SIDE = 0xf0, CELL_UNIT = 0xf8,
    CELLS = 17, PER_REC = 32, CELLS_MAX = 8192, NEUT = 0x5455454e
};
typedef u64 (*Capture)(Object *tm, char *cell, u32 side, char show);
static Capture g_capture;
static Object *g_tm;
static bool g_tried, g_ok, g_enabled = true, g_applying;
static u32 g_sent[CELLS_MAX], g_hold[CELLS_MAX];
static bool g_have_hold;
static DWORD g_last_scan;
static u64 g_stat_sent, g_stat_set, g_stat_forced, g_stat_refused;

static bool ready(void)
{
    return g_ok && session_connected() && session_versus_or_coop() && session_host_auth();
}
static bool joining(void) { return ready() && session_role() == ROLE_JOIN; }

static char *cell_table(u32 *n)
{
    char *t = (char *)g_tm;
    if (!readable(t + TM_CELLS, 0x10)) return 0;
    char *cells = *(char **)(t + TM_CELLS);
    *n = *(u32 *)(t + TM_CELL_N);
    if (!cells || *n > CELLS_MAX || (*n && !readable(cells, (size_t)*n * CELL_REC))) return 0;
    return cells;
}

// The joining game's own captures are the host's to make.
static u64 capture_hook(Object *tm, char *cell, u32 side, char show)
{
    if (g_applying || tm != g_tm || !joining()) return g_capture(tm, cell, side, show);
    // the holder confirmed (the scan does that for every cell; it updates what this player is shown): the game's own
    if (readable(cell + CELL_OWNER, 4) && *(u32 *)(cell + CELL_OWNER) == side) return g_capture(tm, cell, side, show);
    // the cell's own capture starts over, as after a capture (else its timer stays full and it asks every step)
    if (readable(cell + CELL_SIDE, 4)) {
        *(u64 *)(cell + CELL_TIMER) = 0;
        *(u32 *)(cell + CELL_SIDE) = 0;
    }
    g_stat_refused++;
    return 0;
}

static void install(ObjMan *om)
{
    g_tried = true;
    char v[8];
    if (GetEnvironmentVariableA("MP_SYNC_CELLS", v, sizeof v) && !atoi(v)) { g_enabled = false; mp_log("cells: each game takes its own ground (MP_SYNC_CELLS=0)"); return; }
    // mov [rsp+8],rbx; mov [rsp+10h],rbp; mov [rsp+18h],rsi; then mov [rsp+20h],rdi (beta16, public) or push rdi/r12 (beta26)
    static const unsigned char sig_old[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18,
                                             0x48, 0x89, 0x7c, 0x24, 0x20 };
    static const unsigned char sig_b26[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18,
                                             0x57, 0x41, 0x54 };
    const char *ag = "adv_game.codelib";
    char *base = build_module(ag);
    Object *tm = base ? engine_find(om, "i_trigman", "trigman") : 0;
    char *fn = base ? build_code(ag, CAPTURE_FN, BV(sig_old, sig_old, sig_b26), BV(sizeof sig_old, sizeof sig_old, sizeof sig_b26)) : 0;
    // (its function table may be endgame.cpp's private copy of the class's by now: the first entry tells)
    if (!tm || !fn || !readable(tm, TM_CHANGES + 4) || !readable(tm->vtbl, 8) || tm->vtbl[0] != ((void **)(base + TM_VTABLE))[0]) {
        mp_log("cells: unknown game build, each game takes its own ground");
        return;
    }
    g_tm = tm;
    u32 n;
    if (!cell_table(&n)) { mp_log("cells: the trigger manager has no cells it can read, each game takes its own ground"); return; }
    g_capture = (Capture)install_detour(fn, 15, capture_hook);
    if (!g_capture) { mp_log("cells: could not be hooked, each game takes its own ground"); return; }
    g_ok = true;
    mp_log("cells: who holds the ground is the host's (%u cells) - its captures are sent and made the same here", n);
}

static void send_changes(bool all)
{
    u32 n;
    char *cells = cell_table(&n);
    if (!cells) return;
    for (u32 at = 0; at < n; at += PER_REC) {
        u32 k = n - at < PER_REC ? n - at : PER_REC;
        bool changed = all;
        for (u32 i = 0; i < k && !changed; i++) changed = *(u32 *)(cells + (size_t)(at + i) * CELL_REC + CELL_OWNER) != g_sent[at + i];
        if (!changed) continue;
        ShotRec r = {};
        r.kind = CELLS;
        r.arg6 = at;
        r.ammo = k;
        for (u32 i = 0; i < k; i++) {
            u32 owner = *(u32 *)(cells + (size_t)(at + i) * CELL_REC + CELL_OWNER);
            memcpy(r.launch + 4 * i, &owner, 4);
            g_sent[at + i] = owner;
        }
        shots_queue(r);
        g_stat_sent += k;
    }
}

// Joining game: every cell as the host holds it.
static void hold(void)
{
    u32 n;
    char *cells = cell_table(&n);
    if (!cells || !g_have_hold) return;
    bool raw = false;
    __try {
        for (u32 i = 0; i < n; i++) {
            u32 want = g_hold[i];
            char *c = cells + (size_t)i * CELL_REC;
            if (!want || *(u32 *)(c + CELL_OWNER) == want) continue;
            *(void **)(c + CELL_UNIT) = 0;          // (no reward: the capturing unit is the host's game's)
            g_applying = true;
            g_capture(g_tm, c, want, 0);
            g_applying = false;
            if (*(u32 *)(c + CELL_OWNER) != want) {
                // a side that has broken captures nothing: the host's word stands all the same
                *(u32 *)(c + CELL_OWNER) = want;
                *(u64 *)(c + CELL_TIMER) = 0;
                *(u32 *)(c + CELL_SIDE) = 0;
                raw = true;
                g_stat_forced++;
            }
            g_stat_set++;
        }
        if (raw) (*(u32 *)((char *)g_tm + TM_CHANGES))++;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_applying = false;
    }
}

void cells_sim_step(ObjMan *om)
{
    if (!g_tried && g_enabled && session_role() != ROLE_NONE && !strcmp(g_program, "tacgame")) install(om);
    if (!ready() || !session_simulating()) return;
    cells_test_step();
    if (session_role() == ROLE_HOST) {
        DWORD now = GetTickCount();
        if (now - g_last_scan >= 250) { g_last_scan = now; send_changes(false); }
    } else {
        hold();
    }
}

void cells_received(const ShotRec &r)
{
    if (r.ammo > PER_REC || r.arg6 >= CELLS_MAX || r.arg6 + r.ammo > CELLS_MAX) return;
    memcpy(g_hold + r.arg6, r.launch, 4 * r.ammo);
    g_have_hold = true;
}

// Host: the changes not sent yet go now (before the end of the battle). Joining game: the host's cells now (before
// its result is worked out).
void cells_flush(void)
{
    if (!ready()) return;
    if (session_role() == ROLE_HOST) send_changes(false);
    else hold();
}

// A new connection: every cell once more.
void cells_reset(void) { memset(g_sent, 0, sizeof g_sent); }

void cells_report(void)
{
    if (g_stat_sent || g_stat_set || g_stat_refused)
        mp_log("cells: 5 s: %llu owners sent; here %llu cells taken as the host has them (%llu of a broken side, written), %llu captures of "
               "this game's own left to the host", g_stat_sent, g_stat_set, g_stat_forced, g_stat_refused);
    g_stat_sent = g_stat_set = g_stat_forced = g_stat_refused = 0;
}

// ---- tests: "cells" (how many cells each side holds here and a hash of all owners), "capture <n>": the n-th cell
// (counted from the first one held by a side, not neutral) to the side that does not hold it, through the game's
// own capture. Carried out on the game thread.
static volatile LONG g_test_list, g_test_capture;
static int g_test_arg;
void cells_test_list(void) { InterlockedExchange(&g_test_list, 1); }
void cells_test_capture(int index) { g_test_arg = index; InterlockedExchange(&g_test_capture, 1); }

void cells_test_step(void)
{
    if (!g_ok || (!g_test_list && !g_test_capture)) return;
    u32 n;
    char *cells = cell_table(&n);
    if (!cells) return;
    u32 sides[2] = { 0, 0 }, held[3] = { 0, 0, 0 };
    for (u32 i = 0; i < n; i++) {
        u32 o = *(u32 *)(cells + (size_t)i * CELL_REC + CELL_OWNER);
        if (!o || o == NEUT) { held[2]++; continue; }
        if (!sides[0] || sides[0] == o) { sides[0] = o; held[0]++; }
        else if (!sides[1] || sides[1] == o) { sides[1] = o; held[1]++; }
    }
    if (InterlockedExchange(&g_test_capture, 0)) {
        int at = 0;
        bool done = false;
        for (u32 i = 0; i < n && !done; i++) {
            char *c = cells + (size_t)i * CELL_REC;
            u32 o = *(u32 *)(c + CELL_OWNER);
            if (!o || o == NEUT || at++ < g_test_arg || !sides[1]) continue;
            u32 to = o == sides[0] ? sides[1] : sides[0];
            g_applying = true;
            g_capture(g_tm, c, to, 1);
            g_applying = false;
            char a[5] = {}, b[5] = {};
            memcpy(a, &o, 4);
            memcpy(b, &to, 4);
            mp_log("test: capture - cell %u from %s to %s: %s", i, a, b, *(u32 *)(c + CELL_OWNER) == to ? "done" : "FAILED");
            done = true;
        }
        if (!done) mp_log("test: capture - no held cell number %d here", g_test_arg);
        g_test_list = 1;
    }
    if (InterlockedExchange(&g_test_list, 0)) {
        const unsigned char *p = (const unsigned char *)cells;
        unsigned h = 2166136261u;
        for (u32 i = 0; i < n; i++)
            for (int k = 0; k < 4; k++) { h ^= p[(size_t)i * CELL_REC + CELL_OWNER + k]; h *= 16777619u; }
        char a[5] = {}, b[5] = {};
        memcpy(a, &sides[0], 4);
        memcpy(b, &sides[1], 4);
        mp_log("test: cells - %u cells: %s %u, %s %u, neutral %u, hash %08x", n, a, held[0], b, held[1], held[2], h);
    }
}
