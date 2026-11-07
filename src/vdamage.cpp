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
#include "protocol.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <unordered_map>
#include <vector>

namespace {
struct Damage { std::vector<float> modules, left; std::vector<unsigned char> gone, tracks; int buttoned = -1; };      // tracks: 8 bytes each
struct Sent { u32 hash; DWORD when; };
}
static std::unordered_map<u64, Damage> g_want;          // joining game: the host's numbers per vehicle
static std::unordered_map<u64, Sent> g_sent;            // host: what was sent last
static bool g_enabled = true, g_checked;
static u64 g_stat_sent, g_stat_bytes, g_stat_set, g_stat_mismatch, g_stat_vehicles;
static DWORD g_last_scan;

enum { TRK_SIZE = 0x2c0, TRK_FLAGS = 0x1c7, TRK_REPAIR = 0x19c, MAX_TRK = 8, MOD_SIZE = 0x78, MOD_DAMAGE = 0x5c, SEC_SIZE = 0x24, SEC_LEFT = 0x1c, SEC_GONE = 0x20, MAX_MOD = 250, MAX_SEC = 230, ROOM = 1150 };      // (one part of either fits a 1200-byte datagram)
// the vehicle's fields (above); NP: not in that build
#define V_MODULES BF(0x1138, 0x1658)
#define V_TRACKS BF(0x1200, NP)
#define V_SECOND BF(0x1db8, 0x2e50)
#define V_BUTTONED BF(0xd0b, 0xd3b)
#define V_END BF(0x1dd0, 0x2e68)               // (all of them lie below this)

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
    if (!on() || session_role() != ROLE_HOST) return;
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
void vdamage_sim_step(ObjMan *)
{
    if (!on() || session_role() != ROLE_JOIN || g_want.empty()) return;
    std::vector<float *> mods;
    for (auto &kv : g_want) {
        Object *o = session_unit(kv.first);
        if (!o || strcmp(o->iface, "i_techn") || !readable(o, V_END)) continue;
        char *v = (char *)o, *sec, *trk;
        u32 nsec, ntrk;
        const Damage &d = kv.second;
        if (d.buttoned >= 0 && (unsigned char)v[V_BUTTONED] != d.buttoned) { v[V_BUTTONED] = (char)d.buttoned; g_stat_set++; }
        // (tracks are sent but not put in: a broken track is more than its flags - where it broke, +0x100 /
        // +0x104, and the state of each of its links, an array at +0x260 - and with the flags alone the link code
        // adv_physic+0x35420 walked into nothing and the game crashed. MP_TEST_VDMG_TRACKS=1 puts them in anyway.)
        static int tracks_too = -1;
        if (tracks_too < 0) { char e[8]; tracks_too = GetEnvironmentVariableA("MP_TEST_VDMG_TRACKS", e, sizeof e) && atoi(e); }
        if (tracks_too && track_list(v, &trk, &ntrk) && ntrk * 8 == d.tracks.size())
            for (u32 k = 0; k < ntrk; k++) {
                char *t = trk + (size_t)k * TRK_SIZE;
                const unsigned char *w = d.tracks.data() + k * 8;
                if (memcmp(t + TRK_FLAGS, w, 4) || memcmp(t + TRK_REPAIR, w + 4, 4)) { memcpy(t + TRK_FLAGS, w, 4); memcpy(t + TRK_REPAIR, w + 4, 4); g_stat_set++; }
            }
        if (!module_list(v, mods) || !second_list(v, &sec, &nsec) || (!d.modules.empty() && mods.size() != d.modules.size()) ||
            (!d.left.empty() && nsec != d.left.size())) { g_stat_mismatch++; continue; }
        for (size_t i = 0; i < d.modules.size(); i++)
            if (*mods[i] != d.modules[i]) { *mods[i] = d.modules[i]; g_stat_set++; }
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
    g_stat_sent = g_stat_bytes = g_stat_set = g_stat_mismatch = 0;
}
