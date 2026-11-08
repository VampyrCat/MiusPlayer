// Vehicle damage (mpmod2). The host's game decides what happens to every vehicle; the joining game carries
// out the same hits (hits.cpp), but a vehicle's damage after a hit depends on where on it the shell struck
// and on everything before, and with the vehicle's saved state not loaded (two PCs that number its model's
// parts differently) the two games drifted apart: after a skirmish fight 8 of 14 vehicles had other modules
// damaged in the joining game. So the damage itself travels, as state - the newest wins:
//
// A vehicle's damage (adv_physic+0x3a2e0 works a hit into it, found with IDA through REA, 2026-10-16):
//   +0x1138  an std::map of module groups; each node's value is a list of module records (0x78 bytes;
//            node +0x28 the records, +0x34 their count). A module's damage is the float at +0x5c, 0..1,
//            the module broken at 1; +0x60 adds up the damage it has taken.
//   +0x1db8  a second list (0x24-byte records, count +0x1dc4): +0x1c what is left of it, +0x20 gone.
//   +0x1200  the tracks (0x2c0 bytes each, count +0x120c): +0x1c7..+0x1ca a broken track's flags, +0x19c how far
//            its repair has got (adv_physic+0x902f0 sets the vehicle's "track broken" bytes +0xd09 / +0xd0a from
//            them every step - with the modules alone a vehicle showed both tracks broken in one game only).
//            Sent, but not yet put into the vehicle (see vdamage_sim_step).
//   +0xd0b   buttoned up (adv_physic+0x31320, the "buttoned" order and the crew's own reaction): the host's too.
// public_beta-26494568 (IDA): the part damage is i_techn slot 340, adv_physic+0x580c0 - modules map +0x1658 (the same
// nodes and 0x78-byte records), the second list +0x2e50 (count +0x2e5c, the same 0x24-byte records); buttoned is
// +0xd3b (i_techn slot 99, +0x3b310, flag 21). That build has new track code (thrown tracks, wheels torn off, a
// repair by men and time: adv_physic+0xe62b0, +0xf7f20, +0x102b80) and no 0x2c0-byte track records any more (not
// mapped): no tracks travel there.
// The host sends these numbers for every vehicle whose damage has changed (and every 3 s for any damaged
// one); the joining game puts them into its copy of the vehicle before every simulation step.
//   PKT_VDMG: Head (flags: 1 the modules are in it, 2 the second list), u64 vehicle, u16 modules, u16 second,
//             f32 damage[modules], u8 buttoned, u8 tracks, (u8 flags[4], f32 repair)[tracks], (f32 left, u8 gone)[second]
//             - the modules and tracks in one datagram and the second list in another when one is too small
// MP_SYNC_VDMG=0 switches it off.
#include "common.h"
#include "build.h"
#include "detour.h"
#include "protocol.h"
#include <algorithm>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <unordered_map>
#include <vector>

namespace {
struct Damage { std::vector<float> modules, left; std::vector<unsigned char> gone, tracks; int buttoned = -1;       // tracks: 8 bytes each
                // (joining game) where this vehicle's module damage values are, walked from its module map once a second:
                // the walk, one node after the other every step for every vehicle, was most of the joining game's time here
                std::vector<float *> at; const void *at_of = 0; DWORD at_when = 0; bool at_ok = false;
                // (joining game) the host's numbers are put in when new ones came, and every half second (nothing here
                // changes them - this game's own hits are dropped); putting them in every step cost a miss per value
                bool fresh = true; DWORD put_at = 0; };
struct Sent { u32 hash; DWORD when; };
}
static std::unordered_map<u64, Damage> g_want;          // joining game: the host's numbers per vehicle
static std::unordered_map<u64, Sent> g_sent;            // host: what was sent last
static bool g_enabled = true, g_checked;
static u64 g_stat_sent, g_stat_bytes, g_stat_set, g_stat_mismatch, g_stat_vehicles;
static DWORD g_last_scan;

#define TRK_SIZE   BF(0x2c0, 0x3f0)          // a track record (beta26: read in IDA, adv_physic +0x4f400 / +0xf7f20)
#define TRK_FLAGS  BF(0x1c7, 0x1cd)          // broken, -, whole track off the wheels, a bare wheel on the ground
#define TRK_REPAIR BF(0x19c, 0x190)          // repair progress (s of work)
enum { MAX_TRK = 8, MOD_SIZE = 0x78, MOD_DAMAGE = 0x5c, SEC_SIZE = 0x24, SEC_LEFT = 0x1c, SEC_GONE = 0x20, MAX_MOD = 250, MAX_SEC = 230, ROOM = 1150 };      // (one part of either fits a 1200-byte datagram)
// the vehicle's fields (above); NP: not in that build
#define V_MODULES BF(0x1138, 0x1658)
#define V_TRACKS BF(0x1200, 0x1720)
#define V_SECOND BF(0x1db8, 0x2e50)
#define V_BUTTONED BF(0xd0b, 0xd3b)
#define V_END BF(0x1e40, 0x2ed8)               // (all of them lie below this)

static bool on(void)
{
    if (!g_checked) { g_checked = true; char v[8]; if (GetEnvironmentVariableA("MP_SYNC_VDMG", v, sizeof v)) g_enabled = atoi(v) != 0; }
    return g_enabled && session_host_auth() && session_connected() && session_simulating() && session_versus_or_coop();
}

// The module records of a vehicle, in the map's order (the same in both games: the same model).
static bool module_list(char *v, std::vector<float *> &out)
{
    out.clear();
    char *head = *(char **)(v + V_MODULES);
    if (!readable(head, 0x40)) return false;
    // in-order walk of an MSVC map: node +0 left, +8 parent, +0x10 right, +0x19 "is the head"
    char *n = *(char **)head;                // leftmost
    for (int guard = 0; n && n != head && guard < 512; guard++) {
        if (!readable(n, 0x40)) return false;
        char *recs = *(char **)(n + 0x28);
        u32 count = *(u32 *)(n + 0x34);
        if (count > MAX_MOD || (count && !readable(recs, (size_t)count * MOD_SIZE))) return false;
        for (u32 i = 0; i < count && out.size() < MAX_MOD; i++) out.push_back((float *)(recs + (size_t)i * MOD_SIZE + MOD_DAMAGE));
        // successor
        char *r = *(char **)(n + 0x10);
        if (r && !r[0x19]) {
            n = r;
            while (true) { char *l = *(char **)n; if (!l || l[0x19]) break; n = l; }
        } else {
            char *p = *(char **)(n + 8);
            while (p && !p[0x19] && n == *(char **)(p + 0x10)) { n = p; p = *(char **)(p + 8); }
            n = p;
        }
        if (n && n[0x19]) break;
    }
    return true;
}

static bool track_list(char *v, char **recs, u32 *count)
{
    if (!V_TRACKS) { *recs = 0; *count = 0; return true; }     // (none to send on that build: the rest still goes)
    *recs = *(char **)(v + V_TRACKS);
    *count = *(u32 *)(v + V_TRACKS + 0xc);
    if (*count > MAX_TRK) return false;
    return !*count || readable(*recs, (size_t)*count * TRK_SIZE);
}

// ---- a track breaking and being repaired: the host's.
// A track breaks only through adv_physic+0x34270(track, wheel, contacts) (beta16 and public alike): a hit on a track
// part (+0x3e590) or a road wheel destroyed (+0x3f0b0) breaks it at that wheel - the link most tied to the wheel in
// the track's contacts (the link manager, global +0xd9d20, slot 12 with the track's link set +0xd8) becomes the break
// point (+0xf8..+0x104), the flags +0x1c7 (broken) / +0x1c8 / +0x1c9 are set, the repair progress +0x19c starts at
// 0 and the links are rebuilt (+0x260). It is repaired inside the vehicle's step (public +0x90280, beta16 +0x902f0):
// with the crew at work the progress grows, and once it reaches the vehicle's repair time (+0x15c8) and none of the
// track's wheels is destroyed the flags are cleared and its wheels take the links again. Writing the flags alone
// (without the break point and the links) made the link code (+0x35420) walk into nothing and crash the joining game.
// So: the host's game reports each break (ShotRec kind 19: the vehicle, the track, the wheel); the joining game breaks
// no track itself and makes the host's breaks through the same function at the same wheel; it takes the host's repair
// progress every step, so that its own step repairs the track when the host's does; a track the host has whole again
// that is still broken here is repaired with the step's own writes.
#define TRK_BREAK      BV(0x34270, 0x34270, 0x4f400)
#define LINK_MGR       BV(0xd9d20, 0xd9d20, 0x191668)
#define TRK_WHEELS     BF(0x200, 0x350)    // the track's wheels: 16-byte entries {wheel, u8 its links on it +8}, count +0xc
#define TRK_LINKSET    BF(0xd8, 0xe0)
#define WHEEL_LINKS_ON BF(0x289, 0x2c1)
#define V_TRACK_FLAG1  BF(0x1c30, 0x2cc8)  // (cleared by a break's callers and by a repair)
#define V_TRACK_FLAG2  BF(0x1e3b, 0x2ed3)  // (cleared by a repair)
#define V_REPAIR_TIME  BV(0x15c8, 0x15c8, 0x1b00)    // the work a repair takes (s; beta26: the repair +0xf7f20)
enum { TRACK_KIND = 19 };
typedef u64 (*TrackBreak)(char *track, char *wheel, void *contacts);
static TrackBreak g_break;
static char *g_physic;
static bool g_trk_tried, g_trk_applying;
static CRITICAL_SECTION g_trk_lock;
static std::vector<ShotRec> g_trk_out, g_trk_in;
static std::unordered_map<u64, DWORD> g_trk_missing;    // joining game: tracks broken there and whole here, since when
static std::unordered_map<u64, DWORD> g_trk_finish;     // ... repaired there and still broken here, since when
static std::unordered_map<u64, DWORD> g_trk_made;       // ... and the host's breaks made here, when (its damage numbers,
                                                        // on the other channel, can still show the track whole for a moment)
static u64 g_stat_breaks_sent, g_stat_breaks_made, g_stat_breaks_dropped, g_stat_repaired, g_stat_progress;

// The vehicle a track belongs to, and the track's number in it.
static Object *track_owner(ObjMan *om, const char *track, u32 *index)
{
    u32 count;
    Object **all = all_objects(om, &count);
    for (u32 i = 0; i < count; i++) {
        Object *o = all[i];
        if (!o || strcmp(o->iface, "i_techn") || !readable(o, V_TRACKS + 0x10)) continue;
        char *recs = *(char **)((char *)o + V_TRACKS);
        u32 n = *(u32 *)((char *)o + V_TRACKS + 0xc);
        if (recs && n <= MAX_TRK && track >= recs && track < recs + (size_t)n * TRK_SIZE && (track - recs) % TRK_SIZE == 0) {
            *index = (u32)((track - recs) / TRK_SIZE);
            return o;
        }
    }
    return 0;
}

static ObjMan *g_trk_om;
static u64 break_hook(char *track, char *wheel, void *contacts)
{
    bool joining = on() && session_role() == ROLE_JOIN;
    if (joining && !g_trk_applying) { g_stat_breaks_dropped++; return 0; }     // the host's breaks come from it
    bool was = track[TRK_FLAGS] != 0;
    u64 r = g_break(track, wheel, contacts);
    if (was || !track[TRK_FLAGS] || g_trk_applying || !on() || session_role() != ROLE_HOST || !g_trk_om) return r;
    u32 index, wheel_index = 0xffffffffu;
    Object *v = track_owner(g_trk_om, track, &index);
    u64 key;
    if (!v || !session_unit_key(v, &key)) return r;
    char *list = *(char **)(track + TRK_WHEELS);
    u32 n = *(u32 *)(track + TRK_WHEELS + 0xc);
    for (u32 k = 0; list && k < n && k < 64; k++)
        if (*(char **)(list + (size_t)k * 16) == wheel) { wheel_index = k; break; }
    ShotRec s = {};
    s.kind = TRACK_KIND;
    s.shooter = key;
    s.ammo = index;
    s.arg6 = wheel_index;
    EnterCriticalSection(&g_trk_lock);
    g_trk_out.push_back(s);
    LeaveCriticalSection(&g_trk_lock);
    return r;
}

static void tracks_install(ObjMan *om)
{
    g_trk_tried = true;
    InitializeCriticalSection(&g_trk_lock);
    g_trk_om = om;
    // push rbx; push r14; sub rsp,28h; cmp byte [rcx+1c7h],0 (beta16 and public)
    static const unsigned char sig[] = { 0x40, 0x53, 0x41, 0x56, 0x48, 0x83, 0xec, 0x28, 0x80, 0xb9, 0xc7, 0x01, 0x00, 0x00, 0x00 };
    // beta26 +0x4f400: mov r11,rsp; push rdi; push r15; sub rsp,0e8h; mov rdi,rcx
    static const unsigned char sig26[] = { 0x4c, 0x8b, 0xdc, 0x57, 0x41, 0x57, 0x48, 0x81, 0xec, 0xe8, 0x00, 0x00, 0x00, 0x48, 0x8b, 0xf9 };
    bool b26 = g_bi == BUILD_BETA26;
    g_physic = build_module("adv_physic.codelib");
    char *fn = TRK_BREAK && g_physic ? build_code("adv_physic.codelib", TRK_BREAK, b26 ? sig26 : sig, b26 ? sizeof sig26 : sizeof sig) : 0;
    if (fn) g_break = (TrackBreak)install_detour(fn, b26 ? sizeof sig26 : sizeof sig, break_hook);
    mp_log("vdamage: %s", g_break ? "a track breaking and its repair are the host's: breaks are sent and made the same here" :
                           "tracks: not known in this build, each game breaks and repairs its own");
}

// Joining game: one of the host's breaks.
static void make_break(const ShotRec &s)
{
    Object *o = session_unit(s.shooter);
    char *trk;
    u32 ntrk;
    if (!o || strcmp(o->iface, "i_techn") || !readable(o, V_END) || !track_list((char *)o, &trk, &ntrk) || s.ammo >= ntrk) return;
    char *track = trk + (size_t)s.ammo * TRK_SIZE;
    if (track[TRK_FLAGS]) return;               // (broken here already)
    char *list = *(char **)(track + TRK_WHEELS);
    u32 n = *(u32 *)(track + TRK_WHEELS + 0xc);
    if (!list || !n || n > 64 || !readable(list, (size_t)n * 16)) return;
    char *wheel = *(char **)(list + (size_t)(s.arg6 < n ? s.arg6 : 0) * 16);
    void *mgr = *(void **)(g_physic + LINK_MGR);
    if (!mgr || !readable(mgr, 8)) return;
    void *contacts = ((void *(*)(void *, u32, u64))(*(void ***)mgr)[12])(mgr, *(u32 *)(track + TRK_LINKSET), 0);
    g_trk_applying = true;
    __try {
        g_break(track, wheel, contacts);
        *(u32 *)((char *)o + V_TRACK_FLAG1) = 0;           // (as the break's callers do)
        g_stat_breaks_made++;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    g_trk_applying = false;
    g_trk_made[s.shooter ^ ((u64)s.ammo << 56)] = GetTickCount();
}

// Joining game: a track the host has whole again, broken here - the writes of the vehicle step's own repair.
static void make_repair(char *v, char *track)
{
    *(unsigned short *)(track + TRK_FLAGS) = 0;
    track[TRK_FLAGS + 2] = 0;
    char *list = *(char **)(track + TRK_WHEELS);
    u32 n = *(u32 *)(track + TRK_WHEELS + 0xc);
    for (u32 k = 0; list && k < n && k < 64; k++) {
        list[(size_t)k * 16 + 8] = 1;
        char *wheel = *(char **)(list + (size_t)k * 16);
        if (readable(wheel, WHEEL_LINKS_ON + 1)) wheel[WHEEL_LINKS_ON] = 1;
    }
    if (g_bi == BUILD_BETA26) {
        *(u32 *)(track + 0x16c) = *(u32 *)(track + 0x180);     // (beta26: the links' phase back, the repair display 0)
        *(u32 *)(track + 0x1d4) = 0;
    }
    *(u32 *)(v + V_TRACK_FLAG1) = 0;
    v[V_TRACK_FLAG2] = 0;
    g_stat_repaired++;
}

void vdamage_track_received(const ShotRec &s)
{
    if (g_trk_in.size() < 256) g_trk_in.push_back(s);
}

static bool second_list(char *v, char **recs, u32 *count)
{
    *recs = *(char **)(v + V_SECOND);
    *count = *(u32 *)(v + V_SECOND + 0xc);
    if (*count > MAX_SEC) return false;
    return !*count || readable(*recs, (size_t)*count * SEC_SIZE);
}

// Host: every 250 ms, the vehicles whose damage has changed.
void vdamage_frame(ObjMan *om, void (*send)(const void *, int))
{
    if (!g_trk_tried && g_enabled && !strcmp(g_program, "tacgame") && session_role() != ROLE_NONE) tracks_install(om);
    if (on()) vdamage_test_step(om);
    if (!on() || session_role() != ROLE_HOST) return;
    if (g_trk_tried) {
        std::vector<ShotRec> out;
        EnterCriticalSection(&g_trk_lock);
        out.swap(g_trk_out);
        LeaveCriticalSection(&g_trk_lock);
        for (ShotRec &s : out) { shots_queue(s); g_stat_breaks_sent++; }
    }
    DWORD now = GetTickCount();
    if (now - g_last_scan < 250) return;
    g_last_scan = now;
    u32 count;
    Object **all = all_objects(om, &count);
    std::vector<float *> mods;
    unsigned char packet[1200];
    for (u32 i = 0; i < count; i++) {
        Object *o = all[i];
        u64 key;
        if (strcmp(o->iface, "i_techn") || !session_unit_key(o, &key) || !readable(o, V_END)) continue;
        char *v = (char *)o, *sec, *trk;
        u32 nsec, ntrk;
        if (!module_list(v, mods) || !second_list(v, &sec, &nsec) || !track_list(v, &trk, &ntrk)) continue;
        u32 h = 2166136261u;
        h = (h ^ (unsigned char)v[V_BUTTONED]) * 16777619u;
        for (u32 k = 0; k < ntrk; k++) { const unsigned char *t = (const unsigned char *)trk + (size_t)k * TRK_SIZE; for (int b = 0; b < 4; b++) h = (h ^ t[TRK_FLAGS + b]) * 16777619u; u32 r; memcpy(&r, t + TRK_REPAIR, 4); h = (h ^ r) * 16777619u; }
        bool damaged = false;
        for (float *m : mods) { u32 b; memcpy(&b, m, 4); h = (h ^ b) * 16777619u; damaged |= *m > 0; }
        for (u32 k = 0; k < nsec; k++) { u32 b; memcpy(&b, sec + (size_t)k * SEC_SIZE + SEC_LEFT, 4); h = (h ^ b ^ (unsigned char)sec[(size_t)k * SEC_SIZE + SEC_GONE]) * 16777619u; }
        Sent &s = g_sent[key];
        bool due = s.hash != h || (damaged && now - s.when > 3000);
        if (!due) continue;
        size_t base = sizeof(Head) + 12, both = base + mods.size() * 4 + 2 + (size_t)ntrk * 8 + (size_t)nsec * 5;
        bool split = both > ROOM;
        for (int part = split ? 1 : 3; part <= 3; part += split ? 1 : 3) {
            if (split && part == 3) break;
            Head hd = make_head(PKT_VDMG);
            hd.flags = (unsigned char)part;               // 1 modules, 2 second list, 3 both
            unsigned char *p = packet;
            memcpy(p, &hd, sizeof hd); p += sizeof hd;
            memcpy(p, &key, 8); p += 8;
            unsigned short nm = (unsigned short)mods.size(), ns = (unsigned short)nsec;
            memcpy(p, &nm, 2); p += 2;
            memcpy(p, &ns, 2); p += 2;
            if (part & 1) {
                for (float *m : mods) { memcpy(p, m, 4); p += 4; }
                *p++ = (unsigned char)v[V_BUTTONED];
                *p++ = (unsigned char)ntrk;
                for (u32 k = 0; k < ntrk; k++) { const char *t = trk + (size_t)k * TRK_SIZE; memcpy(p, t + TRK_FLAGS, 4); memcpy(p + 4, t + TRK_REPAIR, 4); p += 8; }
            }
            if (part & 2) for (u32 k = 0; k < nsec; k++) { memcpy(p, sec + (size_t)k * SEC_SIZE + SEC_LEFT, 4); p += 4; *p++ = (unsigned char)sec[(size_t)k * SEC_SIZE + SEC_GONE]; }
            send(packet, (int)(p - packet));
            g_stat_bytes += p - packet;
        }
        s.hash = h;
        s.when = now;
        g_stat_sent++;
    }
}

void vdamage_on_packet(const unsigned char *data, int size)
{
    if (session_role() != ROLE_JOIN || size < (int)sizeof(Head) + 12) return;
    int part = ((const Head *)data)->flags ? ((const Head *)data)->flags : 3;
    const unsigned char *p = data + sizeof(Head);
    u64 key;
    unsigned short nm, ns;
    memcpy(&key, p, 8);
    memcpy(&nm, p + 8, 2);
    memcpy(&ns, p + 10, 2);
    if (nm > MAX_MOD || ns > MAX_SEC || size < (int)(sizeof(Head) + 12 + ((part & 1) ? nm * 4 + 2 : 0) + ((part & 2) ? ns * 5 : 0))) return;
    const unsigned char *end = data + size;
    p += 12;
    Damage &d = g_want[key];
    d.fresh = true;
    if (part & 1) {
        d.modules.resize(nm);
        memcpy(d.modules.data(), p, nm * 4ull);
        p += nm * 4;
        d.buttoned = *p++;
        unsigned nt = *p++;
        if (nt > MAX_TRK || p + nt * 8 > end) return;
        d.tracks.assign(p, p + nt * 8);
        p += nt * 8;
    }
    if ((part & 2) && p + ns * 5 > end) return;
    if (part & 2) {
        d.left.resize(ns);
        d.gone.resize(ns);
        for (unsigned k = 0; k < ns; k++) { memcpy(&d.left[k], p, 4); d.gone[k] = p[4]; p += 5; }
    }
}

// Joining game: before every step, the host's numbers into the vehicles.
void vdamage_sim_step(ObjMan *om)
{
    if (!on() || session_role() != ROLE_JOIN) return;
    if (!g_trk_tried && g_enabled) tracks_install(om);
    if (g_break && !g_trk_in.empty()) {
        std::vector<ShotRec> in;
        in.swap(g_trk_in);
        for (const ShotRec &s : in) make_break(s);
    }
    if (g_want.empty()) return;
    for (auto &kv : g_want) {
        Object *o = session_unit(kv.first);
        if (!o || strcmp(o->iface, "i_techn") || !readable(o, V_END)) continue;
        char *v = (char *)o, *sec, *trk;
        u32 nsec, ntrk;
        const Damage &d = kv.second;
        if (d.buttoned >= 0 && (unsigned char)v[V_BUTTONED] != d.buttoned) { v[V_BUTTONED] = (char)d.buttoned; g_stat_set++; }
        // Tracks (see break_hook): a track broken in both games takes the host's repair progress, so this game's own
        // step repairs it when the host's does; one the host has whole again is repaired here at once. (Breaks come
        // as events, made through the game's own function.)
        if (g_break && track_list(v, &trk, &ntrk) && ntrk * 8 == d.tracks.size())
            for (u32 k = 0; k < ntrk; k++) {
                char *t = trk + (size_t)k * TRK_SIZE;
                const unsigned char *w = d.tracks.data() + k * 8;
                bool broken_there = w[0] != 0, broken_here = t[TRK_FLAGS] != 0;
                u64 mk = kv.first ^ ((u64)k << 56);
                float need = *(float *)(v + V_REPAIR_TIME);
                if (broken_there && broken_here) {
                    // the host's progress, kept just short of done: only the host's word finishes a repair here
                    float host;
                    memcpy(&host, w + 4, 4);
                    if (need > 1.0f && host > need - 1.0f) host = need - 1.0f;
                    if (*(float *)(t + TRK_REPAIR) != host) { *(float *)(t + TRK_REPAIR) = host; g_stat_progress++; }
                    g_trk_finish.erase(mk);
                } else if (!broken_there && broken_here) {
                    auto made = g_trk_made.find(mk);
                    if (made == g_trk_made.end() || GetTickCount() - made->second > 3000) {
                        // repaired there: the progress made full, so that this game's own step repairs it; if it has
                        // not within half a second (a wheel lost here and not there, say), the repair's writes here
                        DWORD now = GetTickCount();
                        auto fin = g_trk_finish.find(mk);
                        if (fin == g_trk_finish.end()) { g_trk_finish[mk] = now; *(float *)(t + TRK_REPAIR) = need; }
                        else if (now - fin->second > 500) { make_repair(v, t); g_trk_finish.erase(fin); }
                        if (made != g_trk_made.end()) g_trk_made.erase(made);
                    }
                } else {
                    g_trk_finish.erase(mk);
                }
                // (broken there, whole here for 2 s - a break that never arrived, as after a new connection: made
                // here at the track's first wheel)
                if (broken_there && !broken_here && g_trk_in.empty()) {
                    DWORD now = GetTickCount();
                    auto it = g_trk_missing.find(mk);
                    if (it == g_trk_missing.end()) g_trk_missing[mk] = now;
                    else if (now - it->second >= 2000) {
                        ShotRec s = {};
                        s.shooter = kv.first;
                        s.ammo = k;
                        s.arg6 = 0;
                        make_break(s);
                        g_trk_missing.erase(it);
                    }
                } else {
                    g_trk_missing.erase(mk);
                }
            }
        Damage &dw = kv.second;
        DWORD tnow = GetTickCount();
        if (!dw.fresh && tnow - dw.put_at < 500) continue;
        dw.fresh = false;
        dw.put_at = tnow;
        if (dw.at_of != v || tnow - dw.at_when >= 1000) {
            dw.at_ok = module_list(v, dw.at);
            dw.at_of = v;
            dw.at_when = tnow;
        }
        const std::vector<float *> &at = dw.at;
        if (!dw.at_ok || !second_list(v, &sec, &nsec) || (!d.modules.empty() && at.size() != d.modules.size()) ||
            (!d.left.empty() && nsec != d.left.size())) { g_stat_mismatch++; continue; }
        for (size_t i = 0; i < d.modules.size(); i++)
            if (*at[i] != d.modules[i]) { *at[i] = d.modules[i]; g_stat_set++; }
        for (u32 k = 0; k < d.left.size(); k++) {
            char *r = sec + (size_t)k * SEC_SIZE;
            if (*(float *)(r + SEC_LEFT) != d.left[k]) { *(float *)(r + SEC_LEFT) = d.left[k]; g_stat_set++; }
            if ((unsigned char)r[SEC_GONE] != d.gone[k]) { r[SEC_GONE] = (char)d.gone[k]; g_stat_set++; }
        }
    }
    g_stat_vehicles = g_want.size();
}

void vdamage_reset(void) { g_want.clear(); g_sent.clear(); }

void vdamage_report(void)
{
    if (g_stat_sent || g_stat_set || g_stat_mismatch)
        mp_log("vdamage: 5 s: %llu vehicles' damage sent (%.1f KB/s), %llu values of the host's put into %llu vehicles here, %llu vehicles not matched (another model here)",
               g_stat_sent, g_stat_bytes / 5120.0, g_stat_set, g_stat_vehicles, g_stat_mismatch);
    if (g_stat_breaks_sent || g_stat_breaks_made || g_stat_breaks_dropped || g_stat_repaired || g_stat_progress)
        mp_log("vdamage: 5 s: tracks - %llu breaks sent; here %llu of the host's breaks made, %llu breaks of this game's own left to the host, "
               "%llu repaired as the host has them, %llu repair progress values taken", g_stat_breaks_sent, g_stat_breaks_made, g_stat_breaks_dropped,
               g_stat_repaired, g_stat_progress);
    g_stat_sent = g_stat_bytes = g_stat_set = g_stat_mismatch = 0;
    g_stat_breaks_sent = g_stat_breaks_made = g_stat_breaks_dropped = g_stat_repaired = g_stat_progress = 0;
}

// Tests: "tracks" - every vehicle's tracks here (broken ones listed), and a hash of which are broken;
// "breaktrack <n>": the n-th vehicle's first track broken through the game's own function, at its first wheel.
// "repairtrack": the first broken track's repair progress set to its vehicle's repair time (+0x15c8), so that the
// vehicle's own step repairs it.
static volatile LONG g_test_tracks, g_test_break, g_test_repair;
static int g_test_break_arg;
void vdamage_test_tracks(void) { InterlockedExchange(&g_test_tracks, 1); }
void vdamage_test_break(int n) { g_test_break_arg = n; InterlockedExchange(&g_test_break, 1); }
void vdamage_test_repair(void) { InterlockedExchange(&g_test_repair, 1); }
void vdamage_test_step(ObjMan *om)
{
    if (!g_break || (!g_test_tracks && !g_test_break && !g_test_repair)) return;
    u32 count;
    Object **all = all_objects(om, &count);
    std::vector<std::pair<u64, Object *>> vs;
    for (u32 i = 0; i < count; i++) {
        u64 key;
        if (all[i] && !strcmp(all[i]->iface, "i_techn") && session_unit_key(all[i], &key) && readable(all[i], V_END)) vs.push_back({ key, all[i] });
    }
    std::sort(vs.begin(), vs.end(), [](const std::pair<u64, Object *> &a, const std::pair<u64, Object *> &b) { return a.first < b.first; });
    if (InterlockedExchange(&g_test_break, 0)) {
        int at = 0;
        bool done = false;
        for (auto &kv : vs) {
            char *trk;
            u32 ntrk;
            if (!track_list((char *)kv.second, &trk, &ntrk) || !ntrk || trk[TRK_FLAGS] || at++ < g_test_break_arg) continue;
            char *list = *(char **)(trk + TRK_WHEELS);
            void *mgr = *(void **)(g_physic + LINK_MGR);
            if (!list || !*(u32 *)(trk + TRK_WHEELS + 0xc) || !mgr) continue;
            void *contacts = ((void *(*)(void *, u32, u64))(*(void ***)mgr)[12])(mgr, *(u32 *)(trk + TRK_LINKSET), 0);
            break_hook(trk, *(char **)list, contacts);      // (as the game's own break: on the host it is sent as any break)
            mp_log("test: breaktrack - vehicle %08x track 0: %s", (u32)kv.first, trk[TRK_FLAGS] ? "broken" : "NOT broken");
            done = true;
            break;
        }
        if (!done) mp_log("test: breaktrack - no vehicle number %d with a whole track", g_test_break_arg);
    }
    if (InterlockedExchange(&g_test_repair, 0)) {
        bool done = false;
        for (auto &kv : vs) {
            char *trk;
            u32 ntrk;
            if (!track_list((char *)kv.second, &trk, &ntrk)) continue;
            for (u32 k = 0; k < ntrk && !done; k++) {
                char *t = trk + (size_t)k * TRK_SIZE;
                if (!t[TRK_FLAGS]) continue;
                *(float *)(t + TRK_REPAIR) = *(float *)((char *)kv.second + V_REPAIR_TIME);
                mp_log("test: repairtrack - vehicle %08x track %u: repair progress set to %.1f", (u32)kv.first, k, *(float *)(t + TRK_REPAIR));
                done = true;
            }
            if (done) break;
        }
        if (!done) mp_log("test: repairtrack - no broken track here");
    }
    if (InterlockedExchange(&g_test_tracks, 0)) {
        u32 h = 2166136261u, broken = 0, tracks = 0;
        for (auto &kv : vs) {
            char *trk;
            u32 ntrk;
            if (!track_list((char *)kv.second, &trk, &ntrk)) continue;
            for (u32 k = 0; k < ntrk; k++) {
                tracks++;
                unsigned char b = trk[(size_t)k * TRK_SIZE + TRK_FLAGS] != 0;
                broken += b;
                unsigned char rec[13] = { b };
                memcpy(rec + 1, &kv.first, 8);
                memcpy(rec + 9, &k, 4);
                for (unsigned char c : rec) { h ^= c; h *= 16777619u; }
                if (b) mp_log("test: track - vehicle %08x track %u broken, repair %.2f", (u32)kv.first, k, *(float *)(trk + (size_t)k * TRK_SIZE + TRK_REPAIR));
            }
        }
        mp_log("test: tracks - %u of %u tracks broken here, hash %08x", broken, tracks, h);
    }
}
