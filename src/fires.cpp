// Fires on the ground (mpmod2). Each game burnt its own fields: a fire's spread is worked out by the fire
// manager with a random generator of its own, seeded differently in each game, so one game's field burnt
// end to end while the other's had a few flames. The host's game now decides where fire burns:
//
// The fire manager (i_fireman, adv_game, vtable +0x12eca0; found with IDA through REA, 2026-10-16) starts every
// fire through its slot 3, adv_game+0x90730(manager, description, flame effect, smoke effect, float) -> fire
// number (-1 none); the 160-byte fire records are at manager +0x208. Shells' fires and the spread alike come
// through it: every 10 s of battle with the "fire wave" option (manager +0x238) adv_game+0x48da0 picks spots
// around each burning fire with the manager's own generator (+0x224, seeded from random at the start),
// weighs the ground's flammability there and starts a fire on each through slot 3 ("flame_sm"/"flame_big",
// "smoke_sm"/"smoke").
//   description: vec4 place, +0x10 flags, +0x14..+0x20 four numbers, +0x24 a number (-1), +0x28 a pointer (0)
// A fire on open ground (+0x24 -1, +0x28 0 - the spread's and a shell's) is the host's: its game sends each one
// it starts (ShotRec kind 14 on the reliable, ordered channel: launch = the description, the two effect names;
// pos[0] = the last argument) and the joining game, which starts none of its own, starts the host's. A fire
// tied to something (+0x24 / +0x28 set: a burning vehicle's) stays each game's own.
// MP_SYNC_FIRES=0 switches it off.
// public_beta-26494568 (IDA): vtable adv_game+0x1bcd30 (six slots there), slot 3 +0xd4820 with the same arguments,
// description and 0xa0-byte records at +0x208; the spread +0x6c9d0 runs with the option +0x238, draws from +0x224
// and starts its fires through slot 3 as before.
#include "common.h"
#include "build.h"
#include "protocol.h"
#include <set>
#include <string>
#include <vector>
#include <string.h>
#include <stdlib.h>

namespace {
typedef u32 (*StartFire)(char *manager, char *desc, const char *flame, const char *smoke, float f);
enum { FIRE_KIND = 14, DESC = 0x30, NAME = 24 };
}
static StartFire g_start;
static void *g_vt_copy[48];
static char *g_manager;
static bool g_enabled = true, g_tried, g_hooked, g_applying;
static u64 g_stat_sent, g_stat_dropped, g_stat_started, g_stat_own;
// (a fire may be started on one of the engine's worker threads: what is to be sent waits here, under the lock,
// and goes out from the next simulation step)
static CRITICAL_SECTION g_lock;
static std::vector<ShotRec> g_out;

static bool on(void)
{
    return g_enabled && g_hooked && session_host_auth() && session_connected() && session_simulating() && session_versus_or_coop();
}

static bool on_open_ground(const char *desc) { return *(const int *)(desc + 0x24) == -1 && *(void *const *)(desc + 0x28) == 0; }

// (the effect names: kept for the battle, as the engine may keep the pointer)
static const char *interned(const char *s)
{
    static std::set<std::string> names;
    return names.insert(std::string(s)).first->c_str();
}

static u32 start_hook(char *manager, char *desc, const char *flame, const char *smoke, float f)
{
    if (g_applying || !on() || !readable(desc, DESC) || !on_open_ground(desc)) {
        if (!g_applying && on()) g_stat_own++;
        return g_start(manager, desc, flame, smoke, f);
    }
    if (session_role() != ROLE_HOST) { g_stat_dropped++; return 0xffffffffu; }     // the host's fires come from it
    u32 id = g_start(manager, desc, flame, smoke, f);
    if (id == 0xffffffffu) return id;
    ShotRec r = {};
    r.kind = FIRE_KIND;
    memcpy(r.launch, desc, DESC);
    if (flame && readable(flame, 1)) strncpy_s((char *)r.launch + DESC, NAME, flame, _TRUNCATE);
    if (smoke && readable(smoke, 1)) strncpy_s((char *)r.launch + DESC + NAME, NAME, smoke, _TRUNCATE);
    r.flags = (flame ? 1 : 0) | (smoke ? 2 : 0);
    r.pos[0] = f;
    EnterCriticalSection(&g_lock);
    g_out.push_back(r);
    LeaveCriticalSection(&g_lock);
    return id;
}

static void install(ObjMan *om)
{
    g_tried = true;
    InitializeCriticalSection(&g_lock);
    char v[8];
    if (GetEnvironmentVariableA("MP_SYNC_FIRES", v, sizeof v)) g_enabled = atoi(v) != 0;
    if (!g_enabled) { mp_log("fires: each game burns its own fires (MP_SYNC_FIRES=0)"); return; }
    Object *fm = engine_find(om, "i_fireman", "fireman");
    char *m = build_module("adv_game.codelib");
    if (!fm || !m || (char *)fm->vtbl != m + BV(0x12eca0, 0x12eca0, 0x1c22b0) || (char *)fm->vtbl[3] != m + BV(0x90730, 0x90730, 0xd7310)) {
        mp_log("fires: unknown game build (or no fire manager) - each game burns its own fires");
        return;
    }
    memcpy(g_vt_copy, fm->vtbl - 1, sizeof g_vt_copy);
    g_start = (StartFire)fm->vtbl[3];
    g_vt_copy[1 + 3] = (void *)start_hook;
    fm->vtbl = g_vt_copy + 1;
    g_manager = (char *)fm;
    g_hooked = true;
    mp_log("fires: fires on open ground are the host's: sent, and started from it in the joining game");
}

// Before every simulation step (from shots_sim_step): the fire manager is hooked once it is there.
void fires_sim_step(ObjMan *om)
{
    if (!g_tried) install(om);
    if (!g_hooked) return;
    std::vector<ShotRec> out;
    EnterCriticalSection(&g_lock);
    out.swap(g_out);
    LeaveCriticalSection(&g_lock);
    for (ShotRec &r : out) { shots_queue(r); g_stat_sent++; }
}

void fires_received(const ShotRec &s, ObjMan *om)
{
    if (!g_tried) install(om);
    if (!g_hooked || !g_start || session_role() == ROLE_HOST) return;
    alignas(16) char desc[DESC + 16];
    memcpy(desc, s.launch, DESC);
    char flame[NAME + 1] = "", smoke[NAME + 1] = "";
    memcpy(flame, s.launch + DESC, NAME);
    memcpy(smoke, s.launch + DESC + NAME, NAME);
    g_applying = true;
    g_start(g_manager, desc, (s.flags & 1) ? interned(flame) : 0, (s.flags & 2) ? interned(smoke) : 0, s.pos[0]);
    g_applying = false;
    g_stat_started++;
}

void fires_report(void)
{
    if (g_stat_sent || g_stat_dropped || g_stat_started || g_stat_own)
        mp_log("fires: 5 s: %llu fires on open ground started here and sent, %llu of the host's started here, %llu of this game's own dropped, %llu tied to something left to this game",
               g_stat_sent, g_stat_started, g_stat_dropped, g_stat_own);
    g_stat_sent = g_stat_dropped = g_stat_started = g_stat_own = 0;
}
