// Battle statistics (mpmod2): what the lower part of the end screen shows - who hit whom, with what, who
// destroyed what - is the host's game's.
//
// The statistics object (i_statistic, adv_game vtable +0x12df90; found with IDA through REA, 2026-10-16) keeps a
// record per squad and per unit (a map at +0x200 by number) and is told of the battle through its virtual slots:
//     3  (+0x18) adv_game+0x68080  (stat, hit record, number)   a hit; its record is the hit's own numbers
//     4  (+0x20) adv_game+0x683e0  (stat, key, unit, flags)     flags OR-ed into a record (84 bytes: what the hit did)
//     6  (+0x30) adv_game+0x68540  (stat, key, which)           one more of a kind (a counter)
//     7  (+0x38) adv_game+0x68560  (stat, killer, victim, which) a kill: the victim's record is marked killed by
//                                                               the killer (+0x70), the killer's lists the victim,
//                                                               counted by the victim's class ("MGUNATR")
//     8  (+0x40) adv_game+0x68700  (stat, key, which)           one more of another kind
//    17  (+0x88) adv_game+0x68820  the summing up when the battle ends (each game, from its own units)
// Each game's own hits feed these, and they follow what each game's own hit did to the unit: in a skirmish
// fight 9 of 43 records ended up different. So slots 3, 4, 6, 7 and 8 are the host's events: the host's game
// sends them (ShotRec.kind 13, on the reliable, ordered channel: ammo = the slot, arg6 = key, ammo_code = unit /
// which, obj2 / shooter = killer / victim, launch = the 84 bytes of flags), and the joining game makes no such
// calls of its own and makes the host's instead. A hit record (slot 3, 0xc8 bytes; beta26 0xd0) travels as two
// of them (ammo 3: its first 0x90 bytes, ammo 103: the rest), its two description pointers (+0x30 the ammunition,
// +0x38 a second one) by their savegame numbers (ammo_check / desc), the part struck (+0x94, a render handle) as
// the victim's (+0x9c) part in this game's numbering. MP_SYNC_STATS=0 switches this off.
// beta26 (read in IDA): vtable +0x1b9ec0, the same slots - 3 +0x9a030, 4 +0x9a470, 6 +0x9a5d0, 7 +0x9a5f0, 8 +0x9a770 -
// doing the same; slot 3 copies 0xd0 bytes of the hit record (0xc8 before).
#include "common.h"
#include "build.h"
#include "protocol.h"
#include <string.h>
#include <stdlib.h>
#include <vector>

typedef void (*Hit)(char *stat, char *record, int number);
typedef void (*Flags)(char *stat, u32 key, int unit, const void *flags);
typedef void (*Count)(char *stat, u32 key, unsigned char which);
// (the killer first: adv_game+0x68560 / beta26 +0x9a5f0 mark the third argument's record "killed by" the second's id)
typedef void (*Kill)(char *stat, Object *killer, Object *victim, unsigned char which);
static Hit g_hit;
static Flags g_flags;
static ObjMan *g_om;
static unsigned char g_hit_first[0x90];          // joining game: the first part of a hit record, waiting for the second
// the hit record slot 3 reads (adv_game+0x68080 copies 0xc8 bytes of it, beta26 +0x9a030 0xd0)
#define HIT_SIZE BF(0xc8, 0xd0)
static bool g_hit_waiting;
static Count g_count6, g_count8;
static Kill g_kill;
static char *g_stat;
static bool g_installed, g_enabled = true, g_injecting;
static u64 g_stat_sent, g_stat_dropped, g_stat_applied, g_stat_missing;
static DWORD g_main_thread;
static CRITICAL_SECTION g_lock;
static std::vector<ShotRec> g_from_threads;        // events made on another thread, sent from the main one

enum { STAT_EVENT = 13, FLAGS_SIZE = 0x54, HIT_SIZE_MAX = 0xd0, HIT_AMMO = 0x30, HIT_DESC2 = 0x38, HIT_PART = 0x94, HIT_VICTIM = 0x9c };

static u32 desc_id(void *p)
{
    if (!p || !g_om || !readable(p, 0x10)) return 0xffffffffu;
    __try { return ((u32 (*)(ObjMan *, void *))g_om->vtbl[OM_SLOT_DESC_ID])(g_om, p); }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return 0xffffffffu; }
}
static void *desc_ptr(u32 id) { return id == 0xffffffffu || !g_om ? 0 : ((void *(*)(ObjMan *, u32))g_om->vtbl[OM_SLOT_DESC_PTR])(g_om, id); }

static bool active(void)
{
    return g_enabled && session_host_auth() && session_connected() && session_simulating() && session_versus_or_coop();
}

static void queue(ShotRec &s)
{
    if (GetCurrentThreadId() == g_main_thread) shots_queue(s);
    else { EnterCriticalSection(&g_lock); g_from_threads.push_back(s); LeaveCriticalSection(&g_lock); }
}

static void send_event(u32 slot, u32 key, u32 arg, u64 killer, u64 victim, const void *flags)
{
    ShotRec s = {};
    s.kind = STAT_EVENT;
    s.ammo = slot;
    s.arg6 = key;
    s.ammo_code = arg;
    s.obj2 = killer;
    s.shooter = victim;
    if (flags && readable(flags, FLAGS_SIZE)) memcpy(s.launch, flags, FLAGS_SIZE);
    queue(s);
    g_stat_sent++;
}

static void hit_hook(char *stat, char *record, int number)
{
    if (!g_injecting && active() && readable(record, HIT_SIZE)) {
        if (session_role() == ROLE_JOIN) { g_stat_dropped++; return; }
        ShotRec a = {}, b = {};
        a.kind = b.kind = STAT_EVENT;
        a.ammo = 3;
        b.ammo = 103;
        memcpy(a.launch, record, 0x90);
        memcpy(b.launch, record + 0x90, HIT_SIZE - 0x90);
        b.arg6 = desc_id(*(void **)(record + HIT_AMMO));
        b.desc3 = desc_id(*(void **)(record + HIT_DESC2));
        b.ammo_code = (u32)number;
        memset(a.launch + HIT_AMMO, 0, 16);
        // (both in a row: the channel is in order, and no other thread's record can come between them here)
        EnterCriticalSection(&g_lock);
        if (GetCurrentThreadId() == g_main_thread) { shots_queue(a); shots_queue(b); }
        else { g_from_threads.push_back(a); g_from_threads.push_back(b); }
        LeaveCriticalSection(&g_lock);
        g_stat_sent++;
    }
    g_hit(stat, record, number);
}

// (the engine calls these on its simulation thread; the channel is queued from there as the hits' are)
static void flags_hook(char *stat, u32 key, int unit, const void *flags)
{
    if (!g_injecting && active()) {
        if (session_role() == ROLE_JOIN) { g_stat_dropped++; return; }
        send_event(4, key, (u32)unit, 0, 0, flags);
    }
    g_flags(stat, key, unit, flags);
}
static void count6_hook(char *stat, u32 key, unsigned char which)
{
    if (!g_injecting && active()) {
        if (session_role() == ROLE_JOIN) { g_stat_dropped++; return; }
        send_event(6, key, which, 0, 0, 0);
    }
    g_count6(stat, key, which);
}
static void count8_hook(char *stat, u32 key, unsigned char which)
{
    if (!g_injecting && active()) {
        if (session_role() == ROLE_JOIN) { g_stat_dropped++; return; }
        send_event(8, key, which, 0, 0, 0);
    }
    g_count8(stat, key, which);
}
static void kill_hook(char *stat, Object *killer, Object *victim, unsigned char which)
{
    if (!g_injecting && active()) {
        if (session_role() == ROLE_JOIN) { g_stat_dropped++; return; }
        u64 k = 0, v = 0;
        if (killer && session_unit_key(killer, &k) && victim && session_unit_key(victim, &v)) send_event(7, 0, which, k, v, 0);
    }
    g_kill(stat, killer, victim, which);
}

static bool patch_slot(void **slot, void *expect, void *hook)
{
    if (*slot != expect) return false;
    DWORD old;
    if (!VirtualProtect(slot, 8, PAGE_READWRITE, &old)) return false;
    *slot = hook;
    VirtualProtect(slot, 8, old, &old);
    return true;
}

void stats_install(ObjMan *om)
{
    if (g_installed) return;
    char *base = build_module("adv_game.codelib");
    if (!base) return;
    g_installed = true;
    g_main_thread = GetCurrentThreadId();
    InitializeCriticalSection(&g_lock);
    char v[8];
    if (GetEnvironmentVariableA("MP_SYNC_STATS", v, sizeof v)) g_enabled = atoi(v) != 0;
    if (!g_enabled) { mp_log("stats: each game counts its own battle statistics (MP_SYNC_STATS=0)"); return; }
    void **vt = (void **)(base + BV(0x12df90, 0x12df90, 0x1bf330));
    if (!readable(vt, 0x50) || vt[3] != base + BV(0x68080, 0x68080, 0x9be50) || vt[4] != base + BV(0x683e0, 0x683e0, 0x9c290) ||
        vt[6] != base + BV(0x68540, 0x68540, 0x9c3f0) || vt[7] != base + BV(0x68560, 0x68560, 0x9c410) || vt[8] != base + BV(0x68700, 0x68700, 0x9c590)) {
        g_enabled = false;
        mp_log("stats: unknown game build, each game counts its own battle statistics");
        return;
    }
    g_hit = (Hit)vt[3];
    g_flags = (Flags)vt[4];
    g_count6 = (Count)vt[6];
    g_kill = (Kill)vt[7];
    g_count8 = (Count)vt[8];
    g_om = om;
    bool ok = patch_slot(&vt[3], (void *)g_hit, (void *)hit_hook) && patch_slot(&vt[4], (void *)g_flags, (void *)flags_hook) && patch_slot(&vt[6], (void *)g_count6, (void *)count6_hook) &&
              patch_slot(&vt[7], (void *)g_kill, (void *)kill_hook) && patch_slot(&vt[8], (void *)g_count8, (void *)count8_hook);
    if (om) g_stat = (char *)engine_find(om, "i_statistic", "statistic");
    mp_log("stats: %s", ok ? "the battle statistics (who hit and destroyed what) are the host's game's" : "could not be hooked");
    if (!ok) g_enabled = false;
}

void stats_received(const ShotRec &s, ObjMan *om)
{
    if (!g_enabled || session_role() != ROLE_JOIN || !session_host_auth()) return;
    if (!g_stat && om) g_stat = (char *)engine_find(om, "i_statistic", "statistic");
    if (!g_stat) return;
    g_injecting = true;
    switch (s.ammo) {
    case 3: memcpy(g_hit_first, s.launch, sizeof g_hit_first); g_hit_waiting = true; break;
    case 103: {
        if (!g_hit_waiting) break;
        g_hit_waiting = false;
        alignas(16) unsigned char rec[HIT_SIZE_MAX + 8] = {};
        memcpy(rec, g_hit_first, 0x90);
        memcpy(rec + 0x90, s.launch, HIT_SIZE - 0x90);
        *(void **)(rec + HIT_AMMO) = desc_ptr(xlate_desc(s.arg6));
        *(void **)(rec + HIT_DESC2) = desc_ptr(xlate_desc(s.desc3));
        // the part struck: a render handle of the victim, in this game's numbering (or none)
        u32 part, victim;
        memcpy(&part, rec + HIT_PART, 4);
        memcpy(&victim, rec + HIT_VICTIM, 4);
        if (part != 0xffffffffu) {
            Object *u = session_unit((1ull << 32) | victim);
            if (!u) u = session_unit((2ull << 32) | victim);
            if (!u || !state_local_handle(u, &part)) part = 0xffffffffu;
            memcpy(rec + HIT_PART, &part, 4);
        }
        if (!*(void **)(rec + HIT_AMMO)) { g_stat_missing++; break; }     // (the engine wants one: slot 3 tests it)
        g_hit(g_stat, (char *)rec, (int)s.ammo_code);
        g_stat_applied++;
        break;
    }
    case 4: g_flags(g_stat, s.arg6, (int)s.ammo_code, s.launch); g_stat_applied++; break;
    case 6: g_count6(g_stat, s.arg6, (unsigned char)s.ammo_code); g_stat_applied++; break;
    case 8: g_count8(g_stat, s.arg6, (unsigned char)s.ammo_code); g_stat_applied++; break;
    case 7: {
        Object *killer = session_unit(s.obj2), *victim = session_unit(s.shooter);
        if (killer && victim) { g_kill(g_stat, killer, victim, (unsigned char)s.ammo_code); g_stat_applied++; }
        else g_stat_missing++;
        break;
    }
    default: break;
    }
    g_injecting = false;
}

// Every frame (main thread): what other threads noted goes out.
void stats_frame(void)
{
    if (!g_installed || !g_enabled) return;
    std::vector<ShotRec> out;
    EnterCriticalSection(&g_lock);
    out.swap(g_from_threads);
    LeaveCriticalSection(&g_lock);
    for (ShotRec &s : out) shots_queue(s);
}

void stats_report(void)
{
    if (g_stat_sent || g_stat_dropped || g_stat_applied || g_stat_missing)
        mp_log("stats: 5 s: %llu statistics events sent, %llu of the host's counted here, %llu of this game's own left out, %llu for units not here",
               g_stat_sent, g_stat_applied, g_stat_dropped, g_stat_missing);
    g_stat_sent = g_stat_dropped = g_stat_applied = g_stat_missing = 0;
}
