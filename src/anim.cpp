// Soldier animation. Every soldier (i_human) has an instance in the animation manager (i_animch, adv_game,
// vtable +0x12f128): a record of 0x140 bytes in the array at manager +0x140, the instance number in the
// soldier at +0x1d60. The soldier's own code tells the manager what it wants through small setters:
//     slot 4   state to be in (stand, lie, run, crawl ... a number in the animation set)   -> record +0xa8
//     slot 11  choose a group of extra animations by name (a crew seat's, say)             -> +0x70
//     slot 12 / 13  play one of the group / stop                                            -> +0x104, +0x10c
//     slot 14  a flag and three numbers, slots 8, 16, 20 a number each, 10, 21, 26 values   -> +0x136,
//              +0x11c, +0x120, +0x124, +0xd4, +0x128, +0x12c, +0x100, +0x114, +0x139
// and the manager moves the figure from the state it is in (+0xac) to the one asked for.
// None of it is in a unit's saved state, and each game's own soldier code decided it for every soldier,
// also for the other player's: measured in a skirmish, 36 to 46 of 131 soldiers were shown in another
// state in the other game at any moment - most of all the moving ones (running in their owner's game,
// lying or crawling in the other: "runs lying down").
//
// What a soldier's animation is asked to do is therefore its owner's:
//   - the owner's game reads these values out of its own soldiers' records and sends them when they change
//     (PKT_ANIM: state, the newest wins; repeated every few seconds);
//   - in the other game the soldier's own code is not let through to the setters, and the owner's values
//     are written into the record instead. Choosing a group by name (slot 11) is let through - its result
//     is used by the caller - and put right from the owner's name afterwards.
//
// The same goes for the way a soldier is turned. A lying soldier faces one of eight directions (soldier
// +0x1e70) and his body's three angles (+0x1210, +0x1214, +0x1218) follow it and the ground at a limited
// rate (the soldier's on-foot update, adv_physic+0x46da0); a soldier on his feet has a heading (+0x1eac,
// +0x1ef8). These are in the saved state, so they did come over - a few times a second, while in between
// this game's own soldier code turned the other player's soldier wherever IT would have him face: he was
// seen turning slowly and snapping back, over and over. The owner's values now come with the animation
// values and are written into the soldier before every simulation step.
// MP_SYNC_ANIM=0 switches all of this off.
//
// public_beta-26494568 (read in IDA): the manager's vtable is adv_game+0x1bd7f8 with one slot more (20, a getter,
// +0xeec80), so the setters 20, 21, 26 above are its slots 21, 22, 27; the records are at manager +0x1c0, still
// 0x140 bytes with every field above where it was (each setter read). The soldier's fields moved (below).
#include "common.h"
#include "build.h"
#include "protocol.h"
#include <algorithm>
#include <stdlib.h>
#include <string.h>
#include <unordered_map>
#include <vector>

typedef u64 (*SetU)(char *, u32, u32);
typedef void (*SetF)(char *, u32, float);
typedef u32 (*Pick)(char *, u32, const char *);
typedef void (*Set0)(char *, u32);
typedef void (*Set14)(char *, u32, unsigned char, float, float, float);
typedef void (*SetB)(char *, u32, unsigned char);

static char *g_mgr;
static void *g_copy[42];
static SetU g_o4, g_o10, g_o12, g_o21;
static SetF g_o8, g_o16, g_o20;
static Pick g_o11;
static Set0 g_o13;
static Set14 g_o14;
static SetB g_o26;
static bool g_on = true, g_facing = true, g_hooked, g_tried;
static std::vector<unsigned char> g_followed;           // by instance number: 1 = a soldier of the other player
struct Soldier { Object *o; u32 id, handle; };
static std::unordered_map<u32, Object *> g_object_of;    // soldier number -> soldier, the other player's
static std::vector<Soldier> g_mine;                      // this game's soldiers
static std::unordered_map<u32, u32> g_handle_of;         // soldier number -> instance number, the other player's
struct AnimSent { AnimRec rec; DWORD when; bool unseen; };       // unseen: the other player had not spotted him (see anim_frame)
static std::unordered_map<u32, AnimSent> g_sent;
static std::unordered_map<u32, AnimRec> g_wanted;        // the owner's newest values per followed soldier
static u32 g_units_seen;
static u32 g_survey_gen, g_wanted_gen;      // count the rebuilds of the maps above / new soldiers in g_wanted (anim_sim_step's list)
static u64 g_stat_dropped, g_stat_sent, g_stat_received, g_stat_groups;

// The records (manager +0x140) and the soldier's fields: his instance number, the direction he lies in (0..7), his
// lying body's three angles and his two headings. beta26: adv_physic i_human slot 233 (+0xa97f0) reads the number
// at +0x21c0; the on-foot update (+0x69ff0) steps the direction at +0x22d0, turns the angles at +0x1290..+0x1298
// and takes the second heading from +0x2458; slot 52 (+0xa8ac0) reads the heading at +0x2330. The 2026-11-08 beta
// (26514238): the second heading +0x24a0, the rest where they were (the manager's code is the same).
#define ANIM_RECORDS BF(0x140, 0x1c0)
#define S_HANDLE BF(0x1d60, 0x21c0)
#define S_DIR8 BF(0x1e70, 0x22d0)
#define S_LIE BF(0x1210, 0x1290)
#define S_HEADING1 BF(0x1eac, 0x2330)
#define S_HEADING2 BF(0x1ef8, 0x24a0)
#define S_END BF(0x1f00, 0x24a8)               // (all of them lie below this)

static bool active(void) { return g_on && g_hooked && session_connected() && session_simulating() && session_versus_or_coop(); }
static inline bool held(u32 h) { return h < g_followed.size() && g_followed[h] && active(); }

static u64 h4(char *m, u32 h, u32 v) { if (held(h)) { g_stat_dropped++; return 0; } return g_o4(m, h, v); }
static u64 h10(char *m, u32 h, u32 v) { if (held(h)) { g_stat_dropped++; return 0; } return g_o10(m, h, v); }
static u64 h12(char *m, u32 h, u32 v) { if (held(h)) { g_stat_dropped++; return 0; } return g_o12(m, h, v); }
static u64 h21(char *m, u32 h, u32 v) { if (held(h)) { g_stat_dropped++; return 0; } return g_o21(m, h, v); }
static void h8(char *m, u32 h, float v) { if (held(h)) { g_stat_dropped++; return; } g_o8(m, h, v); }
static void h16(char *m, u32 h, float v) { if (held(h)) { g_stat_dropped++; return; } g_o16(m, h, v); }
static void h20(char *m, u32 h, float v) { if (held(h)) { g_stat_dropped++; return; } g_o20(m, h, v); }
static void h13(char *m, u32 h) { if (held(h)) { g_stat_dropped++; return; } g_o13(m, h); }
static void h14(char *m, u32 h, unsigned char f, float a, float b, float c) { if (held(h)) { g_stat_dropped++; return; } g_o14(m, h, f, a, b, c); }
static void h26(char *m, u32 h, unsigned char v) { if (held(h)) { g_stat_dropped++; return; } g_o26(m, h, v); }

static char *record(u32 handle)
{
    char *base = *(char **)(g_mgr + ANIM_RECORDS);
    char *r = base + (size_t)handle * 0x140;
    return handle < 100000 && readable(r, 0x140) ? r : 0;
}

// The name of the group a record has chosen: the key of the map node the group sits in, 0x3e bytes before it.
static const char *group_name(const char *rec)
{
    const char *g = *(const char *const *)(rec + 0x70);
    if (!g || !readable(g - 0x3e, 0x40)) return "";
    return g - 0x3e;
}

// The soldier's own values for how he is turned.
static void read_soldier(const Object *o, AnimRec &a)
{
    const char *s = (const char *)o;
    memcpy(&a.dir8, s + S_DIR8, 4);
    memcpy(&a.lie[0], s + S_LIE, 4);
    memcpy(&a.lie[1], s + S_LIE + 4, 4);
    memcpy(&a.lie[2], s + S_LIE + 8, 4);
    memcpy(&a.heading[0], s + S_HEADING1, 4);
    memcpy(&a.heading[1], s + S_HEADING2, 4);
}

static void write_soldier(Object *o, const AnimRec &a)
{
    char *s = (char *)o;
    if (a.dir8 < 8) memcpy(s + S_DIR8, &a.dir8, 4);
    for (int i = 0; i < 3; i++) if (a.lie[i] == a.lie[i]) memcpy(s + S_LIE + i * 4, &a.lie[i], 4);     // (not a NaN)
    if (a.heading[0] == a.heading[0]) memcpy(s + S_HEADING1, &a.heading[0], 4);
    if (a.heading[1] == a.heading[1]) memcpy(s + S_HEADING2, &a.heading[1], 4);
}

static void read_record(const char *r, AnimRec &a)
{
    memcpy(&a.want, r + 0xa8, 4);
    memcpy(&a.play, r + 0x104, 4);
    memcpy(&a.variant, r + 0x10c, 4);
    memcpy(&a.p100, r + 0x100, 4);
    memcpy(&a.p114, r + 0x114, 4);
    memcpy(&a.d4, r + 0xd4, 4);
    memcpy(&a.a, r + 0x11c, 12);
    memcpy(&a.p128, r + 0x128, 8);
    a.b136 = (unsigned char)r[0x136];
    a.b139 = (unsigned char)r[0x139];
    const char *name = group_name(r);
    size_t n = strnlen(name, sizeof a.group);
    a.has_group = n > 0 && n < sizeof a.group;           // (a longer name is not passed on: the group stays local)
    memset(a.group, 0, sizeof a.group);
    if (a.has_group) memcpy(a.group, name, n);
}

static void write_record(char *r, u32 handle, const AnimRec &a)
{
    if (a.has_group && strncmp(group_name(r), a.group, sizeof a.group)) {
        char name[sizeof a.group + 1] = "";
        memcpy(name, a.group, sizeof a.group);
        g_o11(g_mgr, handle, name);
        g_stat_groups++;
    }
    memcpy(r + 0xa8, &a.want, 4);
    memcpy(r + 0x100, &a.p100, 4);
    memcpy(r + 0x114, &a.p114, 4);
    memcpy(r + 0xd4, &a.d4, 4);
    memcpy(r + 0x11c, &a.a, 12);
    memcpy(r + 0x128, &a.p128, 8);
    r[0x136] = (char)a.b136;
    r[0x139] = (char)a.b139;
    // one of the group plays only if this game's record has that group and it has that many
    const char *g = *(const char *const *)(r + 0x70);
    bool can = a.play && g && readable(g, 0x10) && a.variant < *(const u32 *)(g + 0xc) && (!a.has_group || !strncmp(group_name(r), a.group, sizeof a.group));
    u32 play = can ? a.play : 0, variant = can ? a.variant : 0xffffffffu;
    memcpy(r + 0x104, &play, 4);
    memcpy(r + 0x10c, &variant, 4);
}

static void install(ObjMan *om)
{
    g_tried = true;
    char v[16];
    if (GetEnvironmentVariableA("MP_SYNC_ANIM", v, sizeof v)) g_on = atoi(v) != 0;
    if (GetEnvironmentVariableA("MP_SYNC_FACING", v, sizeof v)) g_facing = atoi(v) != 0;      // 0: how a soldier is turned stays each game's own
    if (!g_on) { mp_log("anim: each game animates every soldier for itself (MP_SYNC_ANIM=0)"); return; }
    Object *mgr = engine_find(om, "i_animch", "animch");
    char *m = build_module("adv_game.codelib");
    // the setters' slots and addresses (adv_game is the same code in both 2024 builds); beta26 read in IDA
    static const unsigned rva_old[] = { 0xa4040, 0xa1350, 0xa13b0, 0xa4ec0, 0xa4ff0, 0xa13d0, 0xa5030, 0xa1420, 0xa1440, 0xa1460, 0xa14c0 };
    static const unsigned rva_26[] = { 0xed9f0, 0xea730, 0xea790, 0xef4c0, 0xef600, 0xea7b0, 0xef640, 0xea800, 0xea820, 0xea840, 0xea8a0 };
    static const int slot_old[] = { 4, 8, 10, 11, 12, 13, 14, 16, 20, 21, 26 }, slot_26[] = { 4, 8, 10, 11, 12, 13, 14, 16, 21, 22, 27 };
    const unsigned *rva = BV(rva_old, rva_old, rva_26);
    const int *slot = BV(slot_old, slot_old, slot_26);
    bool known = mgr && m && (char *)mgr->vtbl == m + BV(0x12f128, 0x12f128, 0x1c2d78);
    for (int i = 0; known && i < _countof(slot_old); i++) known = (char *)mgr->vtbl[slot[i]] == m + rva[i];
    if (!known) { mp_log("anim: unknown game build, each game animates every soldier for itself"); return; }
    void **vt = mgr->vtbl;
    memcpy(g_copy, vt - 1, sizeof g_copy);
    g_o4 = (SetU)vt[slot[0]]; g_o8 = (SetF)vt[slot[1]]; g_o10 = (SetU)vt[slot[2]]; g_o11 = (Pick)vt[slot[3]]; g_o12 = (SetU)vt[slot[4]];
    g_o13 = (Set0)vt[slot[5]]; g_o14 = (Set14)vt[slot[6]]; g_o16 = (SetF)vt[slot[7]]; g_o20 = (SetF)vt[slot[8]]; g_o21 = (SetU)vt[slot[9]];
    g_o26 = (SetB)vt[slot[10]];
    g_copy[1 + slot[0]] = h4; g_copy[1 + slot[1]] = h8; g_copy[1 + slot[2]] = h10; g_copy[1 + slot[4]] = h12; g_copy[1 + slot[5]] = h13;
    g_copy[1 + slot[6]] = h14; g_copy[1 + slot[7]] = h16; g_copy[1 + slot[8]] = h20; g_copy[1 + slot[9]] = h21; g_copy[1 + slot[10]] = h26;
    g_mgr = (char *)mgr;
    mgr->vtbl = g_copy + 1;
    g_hooked = true;
    mp_log("anim: what a soldier's animation is asked to do is its owner's: sent, and followed here");
}

// Which instance is whose (again whenever the number of objects has changed, and every second).
static void survey(ObjMan *om)
{
    u32 count;
    Object **all = all_objects(om, &count);
    static DWORD last;
    static u32 phase_seen;
    DWORD now = GetTickCount();
    // (again at once when the battle's phase changes: whose a soldier is changes at the end of deployment)
    if (count == g_units_seen && now - last < 1000 && session_phase() == phase_seen) return;
    phase_seen = session_phase();
    g_units_seen = count;
    last = now;
    g_survey_gen++;
    g_mine.clear();
    g_handle_of.clear();
    g_object_of.clear();
    std::fill(g_followed.begin(), g_followed.end(), 0);
    for (u32 i = 0; i < count; i++) {
        Object *o = all[i];
        u64 key;
        if (strcmp(o->iface, "i_human") || !session_unit_key(o, &key)) continue;
        u32 handle = *(u32 *)((char *)o + S_HANDLE);
        if (handle >= 100000) continue;
        if (session_owns(o)) {
            Soldier s = { o, (u32)key, handle };
            g_mine.push_back(s);
        } else {
            if (g_followed.size() <= handle) g_followed.resize((size_t)handle + 256, 0);
            g_followed[handle] = 1;
            g_handle_of[(u32)key] = handle;
            g_object_of[(u32)key] = o;
        }
    }
}

// Before a simulation step: the owner's values go into the records of the other player's soldiers.
void anim_sim_step(ObjMan *om)
{
    if (!g_tried) install(om);
    if (!active()) return;
    survey(om);
    // Through a flat list of the followed soldiers, made again when the maps change: two look-ups by soldier number for
    // each of them every step took the joining game a few ms a second in an operation battle. (g_wanted's values stay
    // where they are when it grows; a new soldier or anim_reset makes the list again.)
    struct Want { const AnimRec *a; u32 handle; Object *o; };
    static std::vector<Want> list;
    static u32 made_survey = ~0u, made_wanted = ~0u;
    if (made_survey != g_survey_gen || made_wanted != g_wanted_gen) {
        made_survey = g_survey_gen;
        made_wanted = g_wanted_gen;
        list.clear();
        for (auto &kv : g_wanted) {
            auto it = g_handle_of.find(kv.first);
            if (it == g_handle_of.end()) continue;
            auto ob = g_object_of.find(kv.first);
            list.push_back({ &kv.second, it->second, ob != g_object_of.end() ? ob->second : 0 });
        }
    }
    for (const Want &w : list) {
        if (char *r = record(w.handle)) write_record(r, w.handle, *w.a);
        if (g_facing && w.o && readable(w.o, S_END) && *(u32 *)((char *)w.o + S_HANDLE) == w.handle) write_soldier(w.o, *w.a);
    }
}

// With the transforms, about 15 times a second: this game's soldiers whose values have changed, and each
// one again every three seconds.
void anim_frame(void (*send)(const void *, int))
{
    if (!active()) return;
    DWORD now = GetTickCount();
    const int k_per_packet = 11;            // 8 + 11 * 104 = 1152 bytes
    unsigned char packet[sizeof(Head) + k_per_packet * sizeof(AnimRec)];
    AnimRec *recs = (AnimRec *)(packet + sizeof(Head));
    int n = 0;
    auto flush = [&]() {
        Head h = make_head(PKT_ANIM, n);
        memcpy(packet, &h, sizeof h);
        send(packet, (int)(sizeof(Head) + n * sizeof(AnimRec)));
        n = 0;
    };
    for (const Soldier &s : g_mine) {
        if (!readable(s.o, S_HANDLE + 8) || *(u32 *)((char *)s.o + S_HANDLE) != s.handle) continue;
        const char *r = record(s.handle);
        if (!r) continue;
        AnimRec a = {};
        a.id = s.id;
        read_record(r, a);
        read_soldier(s.o, a);
        // What he is asked to do goes out at once when it changes; the numbers that drift all the time
        // (how he is turned, the animation's parameters) when they have moved, with the transforms (15 times a
        // second at most). The animation's numbers are how it plays (adv_game+0xa49a0 reads them): +0x11c and
        // +0x120 scale how fast its two layers run, +0x124 blends them, and +0xd4 at nought holds the animation.
        // Sent only on a change of 0.03 and seven times a second, a soldier in the other game played on at a
        // speed of before or stood frozen for most of a second (his animation's clocks did not move while his
        // owner's ran) - his legs moving in fits and starts. A number going to or from nought goes out at once.
        AnimSent &was = g_sent[s.id];
        const AnimRec &b = was.rec;
        auto zero = [](float x) { return x < 0.0001f && x > -0.0001f; };
        bool asked = a.want != b.want || a.play != b.play || a.variant != b.variant || a.p100 != b.p100 || a.p114 != b.p114 ||
                     a.b136 != b.b136 || a.b139 != b.b139 || a.has_group != b.has_group || a.dir8 != b.dir8 || memcmp(a.group, b.group, sizeof a.group) ||
                     zero(a.d4) != zero(b.d4) || zero(a.a) != zero(b.a) || zero(a.b) != zero(b.b);
        auto apart = [](float x, float y, float eps) { float d = x - y; return d > eps || d < -eps || (x != x) != (y != y); };
        bool drifted = apart(a.heading[0], b.heading[0], 0.01f) || apart(a.heading[1], b.heading[1], 0.01f) ||
                       apart(a.lie[0], b.lie[0], 0.01f) || apart(a.lie[1], b.lie[1], 0.01f) || apart(a.lie[2], b.lie[2], 0.01f) ||
                       apart(a.d4, b.d4, 0.005f) || apart(a.a, b.a, 0.005f) || apart(a.b, b.b, 0.005f) || apart(a.c, b.c, 0.005f) ||
                       apart(a.p128, b.p128, 0.005f) || apart(a.p12c, b.p12c, 0.005f);
        bool exact = !memcmp(&was.rec, &a, sizeof a);
        // A soldier of the host's side whom the other player has not spotted is not drawn in his game (versus,
        // session_other_sees): only what he is asked to do goes at once, the drifting numbers with the three-second
        // repeat - and everything at once when he is spotted. Stepanovka: animation values were ~250 KB/s of the host's
        // traffic, sent for every soldier alike. MP_ANIM_UNSEEN=0: as for the others.
        static int unseen_slow = -1;
        if (unseen_slow < 0) { char v[8]; unseen_slow = !GetEnvironmentVariableA("MP_ANIM_UNSEEN", v, sizeof v) || atoi(v) != 0; }
        bool unseen = unseen_slow && !session_other_sees(s.o);
        bool spotted = was.unseen && !unseen;
        was.unseen = unseen;
        if (unseen) drifted = false;
        if (!asked && !drifted && !spotted && !(!exact && !unseen && now - was.when >= 1000) && now - was.when < 3000 + (s.id % 32) * 40) continue;
        was.rec = a;
        was.when = now;
        recs[n++] = a;
        g_stat_sent++;
        if (n == k_per_packet) flush();
    }
    if (n) flush();
}

void anim_on_packet(const unsigned char *data, int size)
{
    if (size < (int)sizeof(Head)) return;
    Head h;
    memcpy(&h, data, sizeof h);
    if ((int)(sizeof(Head) + h.count * sizeof(AnimRec)) > size) return;
    for (unsigned i = 0; i < h.count; i++) {
        AnimRec a;
        memcpy(&a, data + sizeof(Head) + i * sizeof(AnimRec), sizeof a);
        auto put = g_wanted.emplace(a.id, a);
        if (put.second) g_wanted_gen++;
        else put.first->second = a;
        g_stat_received++;
    }
}

// A new connection: everything is sent again, and nothing of the last one is kept.
void anim_reset(void)
{
    g_sent.clear();
    g_wanted.clear();
    g_wanted_gen++;
}

void anim_report(void)
{
    if (!g_stat_sent && !g_stat_received && !g_stat_dropped) return;
    mp_log("anim: 5 s: %llu soldiers' animation values sent, %llu received, %llu of this game's own orders for the other player's soldiers held back, %llu groups chosen from the owner's name",
           g_stat_sent, g_stat_received, g_stat_dropped, g_stat_groups);
    g_stat_sent = g_stat_received = g_stat_dropped = g_stat_groups = 0;
}
