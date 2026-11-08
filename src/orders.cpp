// Orders (mpmod2). In a battle with another player the host's game is the only one whose simulation
// counts: it owns every unit (session.cpp, session_owns) and the joining game shows what it sends. The
// joining player still commands his own forces - his orders are carried over to the host's game here.
//
// Every order a player gives in battle reaches the units through one call, the formation manager's
// virtual slot 16, adv_game+0xd3910 (found with IDA through REA, 2026-10-16):
//     issue(i_formation, record, units, settings, queue, arg) -> order id (-1: none)
//   record    48 bytes: three vec4 (places, directions); the order's kind at +0x28 (names: k_kinds)
//   units     the engine's list: pointer array at +0, count at +0xc
//   settings  20 bytes the formation copies (from the interface's 148-byte order presets), or none
//   queue     1: after the orders the units already have (waypoints), 0: instead of them
//   arg       -1 from the interface
// The interface reaches it from its order worker (adv_interf+0x13f20, returning to +0x13fa6) - every
// do_ord_* button, path, placement (plc_*) and support call - and once on its own: the "reverse" order
// (adv_interf+0x13bc0, returning to +0x13eb2) first puts every unit into reverse (unit vtable +0x590, 1)
// and then orders the move. Calling off-map support is an order too; placing units on the deployment screen
// is not (in a skirmish nothing of it came through here) - each player's game owns his units until deployment
// ends (session.cpp, host_decides).
//
// Joining game: the call runs as always (the interface shows the order at once, and these units are
// followed from the host anyway), and the order goes to the host on the shots' reliable, ordered channel:
//     ShotRec.kind 11  the order: launch = record, settings, up to 9 unit numbers; obj2 = its serial,
//                      arg6 = how many units, ammo = flags (1 settings, 2 queue, 4 reverse), weapon = which
//                      eight-byte words of the record held one of the units (sent as its number),
//                      ammo_code = the order's id in the joining game, shooter = arg
//     ShotRec.kind 12  more of its units: obj2 = serial, arg6 = index of the first, ammo = how many (<= 18)
// Host: the order is carried out through the same call for the units of the joining player's that it
// names (a unit of anybody else is left out).
//
// MP_LOG_ORDERS=1 logs every order either game's interface gives, with its record.
//
// beta26 (public_beta-26494568; read in IDA): the order call is adv_game+0x144d10 (slot 16, the same arguments,
// record and first bytes); the interface's order worker +0x14420 returns from it to +0x144a3, the reverse routine
// +0x14050 to +0x143a7, and puts its units into reverse with unit vtable +0x5a0. The worker there first refuses
// every order while a flag of its own says so (+0x13fc0) - the order then never reaches the call.
#include "common.h"
#include "build.h"
#include "protocol.h"
#include "detour.h"
#include <intrin.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unordered_map>
#include <vector>

namespace {         // (types of this file only: another file has an Incoming of its own)
struct EngList { Object **data; u32 a8, count, cap, pad; };
typedef u32 (*Issue)(char *formation, char *record, EngList *units, char *settings, char queue, int arg);
static Issue g_issue;
static ObjMan *g_om;
static char *g_formation;
static bool g_installed, g_log, g_applying;
static u64 g_serial;
static size_t g_interf_base, g_interf_size;

enum { ORDER_HEAD = 11, ORDER_UNITS = 12, KEYS_IN_HEAD = 9, KEYS_IN_MORE = 18 };
enum { O_SETTINGS = 1, O_QUEUE = 2, O_REVERSE = 4 };
// adv_interf: where the order worker and the reverse routine come back from the order call, and the worker itself
#define WORKER_RETURN  BV(0x13fa6, 0x13fa6, 0x14593)
#define REVERSE_RETURN BV(0x13eb2, 0x13eb2, 0x14497)
#define ORDER_WORKER   BV(0x13f20, 0x13f20, 0x14510)
// unit vtable: put into reverse (arg 1; beta26 from the reverse routine), alive enough to be selected,
// "this player commands the squad" (beta26 from the box selection +0x139020)
#define ORD_VT_REVERSE    BF(0x590, 0x5a0)
#define ORD_VT_SELECTABLE BF(0x350, 0x358)
#define ORD_VT_COMMANDED  BF(0x550, 0x558)
// soldier (adv_physic): the vehicle he sits in (0 on foot), his station in it (-1 none) - as the soldier's slot 182
// (+0x62730) returns them, beta26 slot 184 (+0xa9130)
#define ORD_HUMAN_VEHICLE BF(0x2388, 0x2c10)
#define ORD_HUMAN_STATION BF(0x1da4, 0x2204)

static const char *const k_kinds[] = {
    "move", "defence", "targets", "sector", "rotate", "trg_gnd", "stop", "wait", "enter", "leave", "crew", "shot",
    "noshot", "shot_inf", "noshot_inf", "shot_air", "noshot_air", "conceal", "noconceal", "ai_ctrl", "noai_ctrl",
    "buttoned", "unbuttoned", "-", "-", "-", "plc_move", "plc_rotate", "plc_enter", "plc_leave", "plc_intrench",
    "mm_rotate", "mm_reverse", "mm_start_sup_art", "mm_start_sup_air", "mm_cancel_trg", "economy", "noeconomy" };
static const char *kind_name(u32 k) { return k < sizeof k_kinds / sizeof *k_kinds ? k_kinds[k] : "?"; }

// an order being put together from its records (host)
struct Incoming { ShotRec head; std::vector<u64> keys; u32 want; };

// Sector, turn and fire-at-ground orders. The interface does not give these through slot 16 but through
// i_formation slots 21-24 (the Mius Framework research, kb/engine/aim-orders; checked in IDA in all three builds):
//   21 (formation, vec4 *drag, float angle, units): a sector - or a turn when the angle is below 10 degrees or the
//      drag shorter than 50 m (adv_interf's drag handler, on release)
//   22 (formation, units): clear the sectors
//   23 (formation, vec4 *point, vec4 *line, units, int tag): a ground target (adv_interf's mouse order)
//   24 (formation, units): clear the ground targets (i_select vt[3], code 5)
// The joining game's calls that come from adv_interf (its player's) go to the host as ShotRec kind 15: flags = the
// slot, pos = the point, launch +0 the vector, +16 the angle, +20 the tag, +72 the first unit numbers as in kind 11
// (more in kind 12 records); the host makes the same call for those of the units that are the joining player's.
enum { AIM_ORDER = 15, AIM_FIRST = 21, AIM_SLOTS = 4 };
#define FORMATION_VT BV(0x12f930, 0x12f930, 0x1c6da8)          // i_formation's vtable (adv_game)
typedef u64 (*AimSector)(char *, const float *, float, EngList *);
typedef u64 (*AimClear)(char *, EngList *);
typedef void (*AimGround)(char *, const float *, const float *, EngList *, int);
static void *g_aim_orig[AIM_SLOTS];
static bool g_aim_test;            // tests: the next call is taken as the player's
}
static u64 g_stat_aim_sent, g_stat_aim_done;
static const char *aim_name(int slot)
{
    return slot == 21 ? "sector or turn" : slot == 22 ? "clear sectors" : slot == 23 ? "ground target" : slot == 24 ? "clear ground targets" : "?";
}
static std::unordered_map<u64, Incoming> g_incoming;
static u64 g_stat_sent, g_stat_done, g_stat_refused, g_stat_units_missing, g_stat_units_not_his;

static bool from_interface(void *ret, bool *reverse)
{
    size_t a = (size_t)ret;
    if (!g_interf_base || a < g_interf_base || a >= g_interf_base + g_interf_size) return false;
    *reverse = a - g_interf_base == (size_t)REVERSE_RETURN;
    return true;
}

static bool forwarding(void) { return session_role() == ROLE_JOIN && session_host_auth() && session_versus_or_coop(); }
// (also asked of any eight bytes of an order record: most are floats, and some happen to be a readable
// address - only a live object of this game with a type name counts)
static bool unit_of(void *p, u64 *key)
{
    if (!p || ((size_t)p & 7) || !readable(p, sizeof(Object))) return false;
    Object *o = (Object *)p;
    if (!g_om || o->objman != g_om || !readable(o->iface, 8)) return false;
    return session_unit_key(o, key) && session_unit(*key) == o;
}

static void log_order(const char *who, const char *record, const EngList *units, const char *settings, char queue, int arg, u32 id, void *ret)
{
    u32 kind = *(const u32 *)(record + 0x28);
    const float *f = (const float *)record;
    char what[256] = "";
    int n = 0;
    for (u32 i = 0; units && i < units->count && i < 4 && n < 200; i++) {
        u64 key;
        n += sprintf_s(what + n, sizeof what - n, " %s", unit_of(units->data[i], &key) ? units->data[i]->full : "?");
    }
    mp_log("order: %s %s (%u) for %u units%s%s queue=%d arg=%d id=%d from %s+0x%llx | %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f | %08x %08x %08x %08x%s",
           who, kind_name(kind), kind, units ? units->count : 0, what, units && units->count > 4 ? " ..." : "", queue, arg, (int)id,
           ret ? "adv_interf" : "-", ret ? (u64)((size_t)ret - g_interf_base) : 0, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7],
           ((const u32 *)record)[8], ((const u32 *)record)[9], ((const u32 *)record)[10], ((const u32 *)record)[11],
           settings ? "" : " (no settings)");
    if (settings) {
        const u32 *s = (const u32 *)settings;
        mp_log("order:   settings %08x %08x %08x %08x %08x", s[0], s[1], s[2], s[3], s[4]);
    }
}

static void forward(char *record, EngList *units, char *settings, char queue, int arg, bool reverse, u32 id)
{
    if (!units || !readable(units, sizeof(EngList)) || !units->count || units->count > 4096 || !readable(units->data, units->count * 8ull)) return;
    std::vector<u64> keys;
    for (u32 i = 0; i < units->count; i++) {
        u64 key;
        if (unit_of(units->data[i], &key)) keys.push_back(key);
    }
    if (keys.empty()) return;
    ShotRec h = {};
    h.kind = ORDER_HEAD;
    h.obj2 = ++g_serial;
    h.arg6 = (u32)keys.size();
    h.ammo = (settings ? O_SETTINGS : 0) | (queue ? O_QUEUE : 0) | (reverse ? O_REVERSE : 0);
    h.ammo_code = id;
    h.shooter = (u64)(u32)arg;
    char rec[48];
    memcpy(rec, record, 48);
    for (int w = 0; w < 6; w++) {                   // a unit named in the record travels as its number
        u64 key;
        if (unit_of(*(void **)(rec + w * 8), &key)) { memcpy(rec + w * 8, &key, 8); h.weapon |= 1u << w; }
    }
    memcpy(h.launch, rec, 48);
    if (settings && readable(settings, 20)) memcpy(h.launch + 48, settings, 20);
    size_t in_head = keys.size() < KEYS_IN_HEAD ? keys.size() : KEYS_IN_HEAD;
    memcpy(h.launch + 72, keys.data(), in_head * 8);
    shots_queue(h);
    for (size_t at = in_head; at < keys.size(); at += KEYS_IN_MORE) {
        ShotRec m = {};
        m.kind = ORDER_UNITS;
        m.obj2 = h.obj2;
        m.arg6 = (u32)at;
        m.ammo = (u32)(keys.size() - at < KEYS_IN_MORE ? keys.size() - at : KEYS_IN_MORE);
        memcpy(m.launch, keys.data() + at, m.ammo * 8ull);
        shots_queue(m);
    }
    g_stat_sent++;
    mp_log("order: %s for %zu units sent to the host (serial %llu)", kind_name(*(u32 *)(record + 0x28)), keys.size(), h.obj2);
}

static u32 issue_hook(char *formation, char *record, EngList *units, char *settings, char queue, int arg)
{
    g_formation = formation;
    u32 id = g_issue(formation, record, units, settings, queue, arg);
    if (g_applying || !readable(record, 48)) return id;
    bool reverse = false;
    void *ret = _ReturnAddress();
    bool player = from_interface(ret, &reverse);
    if (g_log && player) log_order(reverse ? "given (reverse)" : "given", record, units, settings, queue, arg, id, ret);
    if (player && forwarding()) forward(record, units, settings, queue, arg, reverse, id);
    return id;
}

// The joining game: one of its player's slot 21-24 calls, to the host.
static void forward_aim(int slot, const float *point, const float *vec, float angle, EngList *units, int tag)
{
    if (!units || !readable(units, sizeof(EngList)) || !units->count || units->count > 4096 || !readable(units->data, units->count * 8ull)) return;
    std::vector<u64> keys;
    for (u32 i = 0; i < units->count; i++) {
        u64 key;
        if (unit_of(units->data[i], &key)) keys.push_back(key);
    }
    if (keys.empty()) return;
    ShotRec h = {};
    h.kind = AIM_ORDER;
    h.flags = (unsigned char)slot;
    h.obj2 = ++g_serial;
    h.arg6 = (u32)keys.size();
    if (point && readable(point, 16)) { memcpy(h.pos, point, 16); h.has_pos = 1; }
    if (vec && readable(vec, 16)) memcpy(h.launch, vec, 16);
    memcpy(h.launch + 16, &angle, 4);
    memcpy(h.launch + 20, &tag, 4);
    size_t in_head = keys.size() < KEYS_IN_HEAD ? keys.size() : KEYS_IN_HEAD;
    memcpy(h.launch + 72, keys.data(), in_head * 8);
    shots_queue(h);
    for (size_t at = in_head; at < keys.size(); at += KEYS_IN_MORE) {
        ShotRec m = {};
        m.kind = ORDER_UNITS;
        m.obj2 = h.obj2;
        m.arg6 = (u32)at;
        m.ammo = (u32)(keys.size() - at < KEYS_IN_MORE ? keys.size() - at : KEYS_IN_MORE);
        memcpy(m.launch, keys.data() + at, m.ammo * 8ull);
        shots_queue(m);
    }
    g_stat_aim_sent++;
    mp_log("order: %s for %zu units sent to the host (serial %llu)", aim_name(slot), keys.size(), h.obj2);
}

// The hooks: the call runs here as always (the interface shows it at once; these units are followed from the
// host anyway) and, when it is the joining player's, it goes to the host as well.
static void aim_given(int slot, void *ret, const float *point, const float *vec, float angle, EngList *units, int tag)
{
    bool test = g_aim_test;
    g_aim_test = false;
    if (g_applying || !forwarding()) return;
    size_t a = (size_t)ret;
    bool player = test || (g_interf_base && a >= g_interf_base && a < g_interf_base + g_interf_size);
    if (g_log) mp_log("order: %s (slot %d) for %u units from %s", aim_name(slot), slot, units && readable(units, 16) ? units->count : 0,
                      player ? "this player" : "this game's AI (not sent: the host's AI gives its own)");
    if (player) forward_aim(slot, point, vec, angle, units, tag);
}

static u64 aim21_hook(char *f, const float *vec, float angle, EngList *units)
{
    g_formation = f;
    u64 r = ((AimSector)g_aim_orig[0])(f, vec, angle, units);
    aim_given(21, _ReturnAddress(), 0, vec, angle, units, -1);
    return r;
}
static u64 aim22_hook(char *f, EngList *units)
{
    g_formation = f;
    u64 r = ((AimClear)g_aim_orig[1])(f, units);
    aim_given(22, _ReturnAddress(), 0, 0, 0, units, -1);
    return r;
}
static void aim23_hook(char *f, const float *point, const float *vec, EngList *units, int tag)
{
    g_formation = f;
    ((AimGround)g_aim_orig[2])(f, point, vec, units, tag);
    aim_given(23, _ReturnAddress(), point, vec, 0, units, tag);
}
static u64 aim24_hook(char *f, EngList *units)
{
    g_formation = f;
    u64 r = ((AimClear)g_aim_orig[3])(f, units);
    aim_given(24, _ReturnAddress(), 0, 0, 0, units, -1);
    return r;
}

// i_formation's vtable entries 21-24 are replaced in place (there is one formation manager), once each holds
// the function of this build (checked).
static void install_aim_hooks(char *ag)
{
    static const size_t old[AIM_SLOTS] = { 0xcd6a0, 0xcd850, 0xcd920, 0xcdaf0 };
    static const size_t b26[AIM_SLOTS] = { 0x13f270, 0x13f430, 0x13f510, 0x13f6f0 };
    void *hooks[AIM_SLOTS] = { (void *)aim21_hook, (void *)aim22_hook, (void *)aim23_hook, (void *)aim24_hook };
    void **vt = (void **)(ag + FORMATION_VT);
    for (int i = 0; i < AIM_SLOTS; i++)
        if (!readable(vt + AIM_FIRST + i, 8) || vt[AIM_FIRST + i] != ag + BV(old[i], old[i], b26[i])) {
            mp_log("orders: unknown game build - sector, turn and fire-at-ground orders stay in the joining player's game");
            return;
        }
    DWORD prot;
    if (!VirtualProtect(vt + AIM_FIRST, AIM_SLOTS * 8, PAGE_READWRITE, &prot)) return;
    for (int i = 0; i < AIM_SLOTS; i++) { g_aim_orig[i] = vt[AIM_FIRST + i]; vt[AIM_FIRST + i] = hooks[i]; }
    VirtualProtect(vt + AIM_FIRST, AIM_SLOTS * 8, prot, &prot);
    mp_log("orders: sector, turn and fire-at-ground orders (i_formation slots 21-24) are hooked too");
}

void orders_install(ObjMan *om)
{
    static const unsigned char sig[] = { 0x4c, 0x89, 0x4c, 0x24, 0x20, 0x4c, 0x89, 0x44, 0x24, 0x18,
                                         0x48, 0x89, 0x54, 0x24, 0x10, 0x48, 0x89, 0x4c, 0x24, 0x08 };
    if (g_installed) return;
    char *ag = build_module("adv_game.codelib"), *ai = build_module("adv_interf.codelib");
    if (!ag || !ai) return;
    g_installed = true;
    g_om = om;
    char v[8];
    g_log = GetEnvironmentVariableA("MP_LOG_ORDERS", v, sizeof v) && atoi(v);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(ai + ((IMAGE_DOS_HEADER *)ai)->e_lfanew);
    g_interf_base = (size_t)ai;
    g_interf_size = nt->OptionalHeader.SizeOfImage;
    // i_formation slot 16 (beta26 +0x144d10: the same first bytes)
    char *fn = build_code("adv_game.codelib", BV(0xd3910, 0xd3910, 0x14a720), sig, sizeof sig);
    if (!fn) { mp_log("orders: unknown game build - the joining player's orders cannot reach the host"); return; }
    g_issue = (Issue)install_detour(fn, sizeof sig, issue_hook);
    mp_log("orders: %s%s", g_issue ? "the order call is hooked - a joining player's orders are carried out by the host's game" : "could not be hooked",
           g_log ? "; every order is logged" : "");
    install_aim_hooks(ag);
}

// The host carries out a complete slot 21-24 order.
static void carry_out_aim(const Incoming &in)
{
    const ShotRec &h = in.head;
    int slot = h.flags;
    if (slot < AIM_FIRST || slot >= AIM_FIRST + AIM_SLOTS || !g_aim_orig[slot - AIM_FIRST]) { g_stat_refused++; return; }
    if (!g_formation && g_om) g_formation = (char *)engine_find(g_om, "i_formation", "formation");
    if (!g_formation) { g_stat_refused++; return; }
    std::vector<Object *> units;
    for (u64 key : in.keys) {
        Object *o = session_unit(key);
        if (!o) { g_stat_units_missing++; continue; }
        if (!session_peer_commands(key)) { g_stat_units_not_his++; continue; }
        units.push_back(o);
    }
    if (units.empty()) {
        g_stat_refused++;
        mp_log("order: %s from the other player refused - none of its %zu units is his here", aim_name(slot), in.keys.size());
        return;
    }
    alignas(16) float point[4], vec[4];
    float angle;
    int tag;
    memcpy(point, h.pos, 16);
    memcpy(vec, h.launch, 16);
    memcpy(&angle, h.launch + 16, 4);
    memcpy(&tag, h.launch + 20, 4);
    EngList list = { units.data(), 0, (u32)units.size(), (u32)units.size(), 0 };
    void *fn = g_aim_orig[slot - AIM_FIRST];
    g_applying = true;
    if (slot == 21) ((AimSector)fn)(g_formation, vec, angle, &list);
    else if (slot == 23) ((AimGround)fn)(g_formation, point, vec, &list, tag);
    else ((AimClear)fn)(g_formation, &list);
    g_applying = false;
    g_stat_aim_done++;
    mp_log("order: %s from the other player carried out for %zu units (serial %llu)", aim_name(slot), units.size(), h.obj2);
}

// The host carries out a complete order.
static void carry_out(const Incoming &in)
{
    const ShotRec &h = in.head;
    if (!g_issue) return;
    if (!g_formation && g_om) g_formation = (char *)engine_find(g_om, "i_formation", "formation");
    if (!g_formation) { g_stat_refused++; return; }
    std::vector<Object *> units;
    for (u64 key : in.keys) {
        Object *o = session_unit(key);
        if (!o) { g_stat_units_missing++; continue; }
        if (!session_peer_commands(key)) { g_stat_units_not_his++; continue; }
        units.push_back(o);
    }
    char rec[48], settings[20];
    memcpy(rec, h.launch, 48);
    memcpy(settings, h.launch + 48, 20);
    for (int w = 0; w < 6; w++) {
        if (!(h.weapon & (1u << w))) continue;
        u64 key;
        memcpy(&key, rec + w * 8, 8);
        Object *o = session_unit(key);
        *(Object **)(rec + w * 8) = o;
    }
    u32 kind = *(u32 *)(rec + 0x28);
    if (units.empty()) {
        g_stat_refused++;
        mp_log("order: %s from the other player refused - none of its %zu units is his here", kind_name(kind), in.keys.size());
        return;
    }
    if (h.ammo & O_REVERSE)
        for (Object *o : units) ((void (*)(Object *, char))o->vtbl[ORD_VT_REVERSE / 8])(o, 1);
    EngList list = { units.data(), 0, (u32)units.size(), (u32)units.size(), 0 };
    g_applying = true;
    u32 id = g_issue(g_formation, rec, &list, (h.ammo & O_SETTINGS) ? settings : 0, (h.ammo & O_QUEUE) ? 1 : 0, (int)(u32)h.shooter);
    g_applying = false;
    g_stat_done++;
    mp_log("order: %s%s from the other player carried out for %zu units (serial %llu, id %d here, %d there)", kind_name(kind),
           (h.ammo & O_REVERSE) ? " (reverse)" : "", units.size(), h.obj2, (int)id, (int)h.ammo_code);
    if (g_log) log_order("carried out", rec, &list, (h.ammo & O_SETTINGS) ? settings : 0, (h.ammo & O_QUEUE) ? 1 : 0, (int)(u32)h.shooter, id, 0);
}

void orders_received(const ShotRec &s)
{
    if (session_role() != ROLE_HOST || !session_host_auth()) return;
    // (an order's pieces are kept apart by the player they came from: each numbers its own orders)
    u64 serial = s.obj2 ^ ((u64)session_order_sender() << 56);
    if (s.kind == ORDER_HEAD || s.kind == AIM_ORDER) {
        Incoming &in = g_incoming[serial];
        in.head = s;
        in.want = s.arg6 > 4096 ? 4096 : s.arg6;
        in.keys.assign(in.want, 0);
        u32 n = in.want < KEYS_IN_HEAD ? in.want : KEYS_IN_HEAD;
        memcpy(in.keys.data(), s.launch + 72, n * 8ull);
        in.want -= n;
    } else if (s.kind == ORDER_UNITS) {
        auto it = g_incoming.find(serial);
        if (it == g_incoming.end()) return;
        Incoming &in = it->second;
        u32 n = s.ammo > KEYS_IN_MORE ? KEYS_IN_MORE : s.ammo;
        if (s.arg6 + n > in.keys.size()) return;
        memcpy(in.keys.data() + s.arg6, s.launch, n * 8ull);
        in.want = in.want > n ? in.want - n : 0;
    } else return;
    auto it = g_incoming.find(serial);
    if (it != g_incoming.end() && !it->second.want) {
        if (it->second.head.kind == AIM_ORDER) carry_out_aim(it->second);
        else carry_out(it->second);
        g_incoming.erase(it);
    }
}

void orders_reset(void) { g_incoming.clear(); }

// ---- tests
// The units of one squad in this game.
static std::vector<Object *> squad_units(u32 squad)
{
    std::vector<Object *> out;
    if (!g_om) return out;
    u32 count;
    Object **all = all_objects(g_om, &count);
    for (u32 i = 0; i < count; i++) {
        u64 key;
        if (session_unit_key(all[i], &key) && ((u32)key & 0x7fffffe0) == (squad & 0x7fffffe0)) out.push_back(all[i]);
    }
    return out;
}

// A soldier sitting in a vehicle (his UNIT_POS means nothing then: the host's game parks it at 16384, 16384).
static bool seated(Object *o)
{
    char *u = (char *)o;
    return !strcmp(o->iface, "i_human") && readable(u, ORD_HUMAN_VEHICLE + 8) && *(void **)(u + ORD_HUMAN_VEHICLE) &&
           *(int *)(u + ORD_HUMAN_STATION) != -1;
}

static bool squad_centre(u32 squad, float *c)
{
    std::vector<Object *> units = squad_units(squad);
    double sum[3] = { 0, 0, 0 };
    int n = 0;
    for (Object *o : units) {
        if (!readable((char *)o + UNIT_POS, 12) || seated(o)) continue;
        const float *p = (const float *)((char *)o + UNIT_POS);
        for (int i = 0; i < 3; i++) sum[i] += p[i];
        n++;
    }
    if (!n) return false;
    for (int i = 0; i < 3; i++) c[i] = (float)(sum[i] / n);
    return true;
}

// Tests: "order <n> <dx> <dz>" - this player's n-th squad is ordered to move by dx, dz metres, through the
// interface's own order worker (adv_interf+0x13f20, beta26 +0x14420), as a click would.
void orders_test_move(int index, float dx, float dz)
{
    std::vector<u32> squads = session_own_squads();
    if (index < 0 || index >= (int)squads.size()) { mp_log("test: order - this player has no squad %d (%zu squads)", index, squads.size()); return; }
    u32 squad = squads[index];
    std::vector<Object *> units = squad_units(squad);
    float c[3];
    char *ai = build_module("adv_interf.codelib");
    Object *ie = g_om ? engine_find(g_om, "i_intereff", "intereff") : 0;
    if (units.empty() || !squad_centre(squad, c) || !ai || !ie) { mp_log("test: order - squad %08x cannot be ordered", squad); return; }
    float len = sqrtf(dx * dx + dz * dz);
    alignas(16) float rec[12] = { c[0] + dx, c[1], c[2] + dz, 1, len > 0 ? dx / len : 1, 0, len > 0 ? dz / len : 0, 0, 0, 0, 0, 0 };
    *(u32 *)&rec[10] = 0;                     // kind 0: move
    EngList list = { units.data(), 0, (u32)units.size(), (u32)units.size(), 0 };
    typedef u32 (*Worker)(Object *, void *, void *, EngList *, void *, char, char);
    u32 id = ((Worker)(ai + ORDER_WORKER))(ie, rec, rec, &list, 0, 0, 0);
    mp_log("test: order - squad %08x (%zu units, centre %.1f %.1f %.1f) ordered to move by %.1f, %.1f: order id %d",
           squad, units.size(), c[0], c[1], c[2], dx, dz, (int)id);
}

// Tests: "selectable" - how many of the units this player commands the game would let him select: the box
// selection (i_formation slot 17, adv_game+0xcce60; beta26 +0x139020) takes a unit when its vtable +0x350 (alive
// enough) and +0x550 (its squad record's +0x3ef: "this player commands the squad") both say yes (beta26 +0x358, +0x558).
void orders_test_selectable(void)
{
    if (!g_om) return;
    u32 count, mine = 0, ok = 0, other_ok = 0;
    Object **all = all_objects(g_om, &count);
    for (u32 i = 0; i < count; i++) {
        u64 key;
        if (!session_unit_key(all[i], &key)) continue;
        void **vt = all[i]->vtbl;
        bool can = ((char (*)(Object *))vt[ORD_VT_SELECTABLE / 8])(all[i]) && ((char (*)(Object *))vt[ORD_VT_COMMANDED / 8])(all[i]);
        if (session_commands_squad((u32)key)) { mine++; ok += can; } else other_ok += can;
    }
    mp_log("test: selectable - %u of this player's %u units can be selected, %u of the other units", ok, mine, other_ok);
}

// Tests: "attack" - every squad of this player is ordered (as above, through the interface's worker) to move
// to the middle of the other side's units, each a little to the side of the last: a fight on demand.
void orders_test_attack(void)
{
    float target[3];
    std::vector<u32> squads = session_own_squads();
    char *ai = build_module("adv_interf.codelib");
    Object *ie = g_om ? engine_find(g_om, "i_intereff", "intereff") : 0;
    // (the other side: the units of squads this player does not command)
    double sum[3] = { 0, 0, 0 };
    int others = 0;
    if (g_om) {
        u32 count;
        Object **all = all_objects(g_om, &count);
        for (u32 i = 0; i < count; i++) {
            u64 key;
            if (!session_unit_key(all[i], &key) || session_commands_squad((u32)key) || !readable((char *)all[i] + UNIT_POS, 12)) continue;
            const float *p = (const float *)((char *)all[i] + UNIT_POS);
            for (int k = 0; k < 3; k++) sum[k] += p[k];
            others++;
        }
    }
    if (others) for (int k = 0; k < 3; k++) target[k] = (float)(sum[k] / others);
    if (!others || squads.empty() || !ai || !ie) { mp_log("test: attack - nothing to attack with or at"); return; }
    typedef u32 (*Worker)(Object *, void *, void *, EngList *, void *, char, char);
    int n = 0;
    for (u32 squad : squads) {
        std::vector<Object *> units = squad_units(squad);
        float c[3];
        if (units.empty() || !squad_centre(squad, c)) continue;
        float tx = target[0] + (float)((n % 5) - 2) * 15.0f, tz = target[2] + (float)((n / 5) % 5 - 2) * 15.0f;
        float dx = tx - c[0], dz = tz - c[2], len = sqrtf(dx * dx + dz * dz);
        alignas(16) float rec[12] = { tx, target[1], tz, 1, len > 0 ? dx / len : 1, 0, len > 0 ? dz / len : 0, 0, 0, 0, 0, 0 };
        *(u32 *)&rec[10] = 0;
        EngList list = { units.data(), 0, (u32)units.size(), (u32)units.size(), 0 };
        ((Worker)(ai + ORDER_WORKER))(ie, rec, rec, &list, 0, 0, 0);
        n++;
    }
    mp_log("test: attack - %d squads of this player ordered to %.0f %.0f", n, target[0], target[2]);
}

// Tests: "place <n> <dx> <dz>" - this player's n-th squad of soldiers on foot (no vehicle, nobody seated) is put
// dx, dz metres away at once, as placing it on the deployment screen does (its soldiers' positions written;
// their height stays).
void orders_test_place(int index, float dx, float dz)
{
    std::vector<u32> squads, all = session_own_squads();
    for (u32 sq : all) {
        std::vector<Object *> us = squad_units(sq);
        bool foot = !us.empty();
        for (Object *o : us) foot = foot && !strcmp(o->iface, "i_human") && !seated(o);
        if (foot) squads.push_back(sq);
    }
    if (index < 0 || index >= (int)squads.size()) { mp_log("test: place - this player has no infantry squad %d (%zu of %zu squads)", index, squads.size(), all.size()); return; }
    u32 squad = squads[index];
    std::vector<Object *> units = squad_units(squad);
    float c[3];
    if (units.empty() || !squad_centre(squad, c)) { mp_log("test: place - squad %08x cannot be placed", squad); return; }
    for (Object *o : units) {
        float *p = (float *)((char *)o + UNIT_POS);
        if (!readable(p, 12)) continue;
        p[0] += dx;
        p[2] += dz;
    }
    mp_log("test: place - squad %08x (%zu units) put from %.2f %.2f %.2f to %.2f %.2f %.2f", squad, units.size(), c[0], c[1], c[2], c[0] + dx, c[1], c[2] + dz);
}

// Tests: "squadpos <squad id>" - where a squad stands in this game.
void orders_test_where(u32 squad)
{
    float c[3];
    if (squad_centre(squad, c)) mp_log("test: squad %08x stands at %.2f %.2f %.2f (%zu units)", squad, c[0], c[1], c[2], squad_units(squad).size());
    else mp_log("test: squad %08x is not in this game", squad);
}

// Tests: "aim <slot> <n> <dx> <dz> <degrees>" - an i_formation slot 21-24 order for this player's n-th squad, given
// as the interface gives it (taken as the player's): 21 a sector (or a turn) along dx, dz from the squad's middle,
// <degrees> wide; 22 clear its sectors; 23 a ground target dx, dz from its middle; 24 clear its ground targets.
void orders_test_aim(int slot, int index, float dx, float dz, float deg)
{
    std::vector<u32> squads = session_own_squads();
    if (slot < AIM_FIRST || slot >= AIM_FIRST + AIM_SLOTS || index < 0 || index >= (int)squads.size() || !g_formation && !g_om) {
        mp_log("test: aim - slot %d / squad %d not possible (%zu squads)", slot, index, squads.size());
        return;
    }
    if (!g_formation) g_formation = (char *)engine_find(g_om, "i_formation", "formation");
    std::vector<Object *> units = squad_units(squads[index]);
    float c[3];
    if (!g_formation || units.empty() || !squad_centre(squads[index], c)) { mp_log("test: aim - squad %d cannot be given it", index); return; }
    alignas(16) float point[4] = { c[0] + dx, c[1], c[2] + dz, 1 }, vec[4] = { dx, 0, dz, 0 };
    EngList list = { units.data(), 0, (u32)units.size(), (u32)units.size(), 0 };
    void **vt = *(void ***)g_formation;          // through the vtable: the hooks, as the interface's calls go
    g_aim_test = true;
    if (slot == 21) ((AimSector)vt[21])(g_formation, vec, deg * 0.01745329f, &list);
    else if (slot == 23) ((AimGround)vt[23])(g_formation, point, vec, &list, -1);
    else ((AimClear)vt[slot])(g_formation, &list);
    g_aim_test = false;
    mp_log("test: aim - %s given to squad %08x (%zu units)", aim_name(slot), squads[index], units.size());
}

void orders_report(void)
{
    if (g_stat_sent || g_stat_done || g_stat_refused || g_stat_units_missing || g_stat_units_not_his || g_stat_aim_sent || g_stat_aim_done)
        mp_log("orders: 5 s: %llu sent to the host (%llu sector/turn/ground target), %llu of the other player's carried out here (%llu sector/turn/ground target), "
               "%llu refused; units left out: %llu not here, %llu not his",
               g_stat_sent + g_stat_aim_sent, g_stat_aim_sent, g_stat_done + g_stat_aim_done, g_stat_aim_done, g_stat_refused, g_stat_units_missing, g_stat_units_not_his);
    g_stat_sent = g_stat_done = g_stat_refused = g_stat_units_missing = g_stat_units_not_his = g_stat_aim_sent = g_stat_aim_done = 0;
}
