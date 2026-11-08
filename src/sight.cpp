// What each player sees of the enemy (mpmod2): the host's spotting decides for both players.
//
// Each game works out its own spotting: i_target (adv_game) casts sight rays between units with a random generator of
// its own, through smoke that each game makes with its own random numbers, on its own timing, and i_visman (adv_game)
// turns that into which enemies the player sees. The joining game's units stand where the host's do, yet its spotting
// agreed with the host's in 0 to 6 % of samples during contact (the Mius Framework research, kb/engine/spotting.md, tests
// 29, 30, 73, 85, 91): a player could see enemies his units on the host had not spotted, or miss the ones they fired at.
//
// i_visman (vtable beta16 / public +0x12ffb0, beta26 +0x1c2dc0) keeps a record per unit (beta16 / public 288 bytes at
// +0x158, beta26 320 at +0x168; count +0xc): unit +0xe8 / +0xf0, side code +0xf4 / +0xfc, enemy +0x115 / +0x11d, own
// +0x116 / +0x11e, not an observer +0x118 / +0x120. From the contacts of its player's side it builds a side list
// (+0x1c0 / +0x1d8) and a list of the selected units' contacts (+0x1d8 / +0x1f0): {entries, count +12, room +16} of
// 24-byte entries {unit, unit, level a, level b} sorted by unit, grown with the CRT's _aligned_malloc / _aligned_free.
// The contacts: observers are the side's records, alive (unit slot 47 > 0), not marked, with an i_target index (unit
// slot 49); i_target (+0xf8) slot 4 gives an observer's contacts, 48-byte entries whose +24 is a pair {unit +0, flags
// +8, levels u16 +0x1a / +0x1c}; a pair counts at level a >= 1000, or b >= 1000 without flags 0xf000; per unit the
// highest levels x 0.001. Older builds: the lists are rebuilt every 5 s (timer +0x1ac) and every tick (+0xe4b20) sets
// the records' levels from them (the selected units' list first), which decide what is drawn. The 2026 beta: every
// 0.5 s (timer +0x1bc) its step (+0x162420) builds the lists (+0x162790), works out each enemy's state from the side
// list (+0x163870) and shows the records (+0x162d00, unit slot 115 with 2 s or 10 s) - all within one call.
//
// Now the host's game builds the joining player's side's list exactly that way, right after its own i_visman rebuilt
// its lists, and sends it (PKT_SIGHT, again every second). The joining game puts it in place of its own side list:
// before every tick on the older builds (and empties the selected units' list, so every level comes from it); on the
// 2026 beta it carries out the 0.5 s step itself with the host's list put in between the build and the state step
// (the game's code, written out from IDA). The rest - levels, states, what is drawn - is the game's own.
// (A port of mius_mp's "sideview", kb/engine/spotting.md sections 3, 8, 9; addresses checked in IDA on beta16 and
// beta26.) MP_SYNC_SIGHT=0 switches it off.
#include "common.h"
#include "build.h"
#include "detour.h"
#include "protocol.h"
#include <algorithm>
#include <string.h>
#include <unordered_map>
#include <vector>

#define VM_VTABLE    BV(0x12ffb0, 0x12ffb0, 0x1c8780)
#define VM_TICK      BV(0xe4b20, 0xe4b20, 0x167fe0)
#define VM_STEP      BV(0, 0, 0x167e30)
#define VM_BUILD     BV(0, 0, 0x1681a0)
#define VM_STATES    BV(0, 0, 0x169280)
#define VM_SHOW      BV(0, 0, 0x168710)
#define VM_ZERO4     BV(0, 0, 0x1f7d60)
#define VM_HALF      BV(0, 0, 0x1cb580)
#define VM_SHORT     BV(0, 0, 0x1cb6a8)
#define VM_LONG      BV(0, 0, 0x1cb7c8)
#define VM_RECS      BF(0x158, 0x168)
#define VR_SIZE      BF(288, 320)
#define VR_UNIT      BF(0xe8, 0xf0)
#define VR_SIDE      BF(0xf4, 0xfc)
#define VR_ENEMY     BF(0x115, 0x11d)
#define VR_OWN       BF(0x116, 0x11e)
#define VR_NOOBS     BF(0x118, 0x120)
#define VM_BATTLE    BF(0x1b4, 0x1c4)
#define VM_TIMER     BF(0x1ac, 0x1bc)
#define VM_SIDE_LIST BF(0x1c0, 0x1d8)
#define VM_SEL_LIST  BF(0x1d8, 0x1f0)
enum {
    VM_TARGET = 0xf8, VM_GAMESPEC = 0x108, VM_BLOCK = 0x1cc, VR_A = 0x30, VR_IDENT = 0x121, VR_STATE = 0x123,
    U_ALIVE = 47, U_TARGET = 49, U_SHOW = 115, U_KEEP = 137, U_LONG = 138, TG_CONTACTS = 4,
    PAIR_FLAGS = 8, PAIR_A = 0x1a, PAIR_B = 0x1c, PER_PACKET = 64, MAX_ENTRIES = 2048, MAX_PARTS = 32
};
#pragma pack(push, 1)
struct SightHead { Head h; unsigned seq, side; unsigned short part, parts; };
struct SightEntry { unsigned long long key; float a, b; };
#pragma pack(pop)
namespace { struct Ent { const void *unit; float a, b; }; }

typedef void (*VisStep)(char *vm, float dt);
typedef void (*VisBuild)(char *vm, char selection);
typedef char (*VisShow)(char *vm, char *rec, char all);
typedef const char *(*Contacts)(void *target, u32 index);
static VisStep g_tick, g_step;
static char *g_vm, *g_ag;
static bool g_tried, g_ok, g_enabled = true;
// host: the list last built for the other player's side, its number, when it went out
static std::vector<SightEntry> g_list;
static std::unordered_map<u64, bool> g_list_keys;      // (the same units, for sight_other_sees)
static unsigned g_seq, g_list_side;
// more players (session.cpp): extra players on the side opposite the other player's get that side's list
static std::vector<SightEntry> g_list2;
static unsigned g_list2_side;
static DWORD g_sent_at;
static float g_timer_seen;
// joining game: the host's list by unit number, put in at every tick
static std::unordered_map<u64, std::pair<float, float>> g_host;
static bool g_have_host;
static unsigned g_host_seq, g_put_seq;          // the host's list held, and the one last put in
static unsigned g_rx_seq, g_rx_parts, g_rx_mask;
static std::vector<SightEntry> g_rx;
static u64 g_stat_built, g_stat_sent, g_stat_put, g_stat_entries, g_stat_grown, g_stat_steps, g_stat_shows;

static bool ready(void) { return g_ok && session_connected() && session_versus_or_coop() && session_host_auth(); }
static bool joining(void) { return ready() && session_role() == ROLE_JOIN && g_have_host; }
static char *recs(u32 *n) { *n = *(u32 *)(g_vm + VM_RECS + 12); return *(char **)(g_vm + VM_RECS); }
static void *slot(void *o, int s) { return (*(void ***)o)[s]; }

// The list memory, as i_visman's own: the CRT the engine uses (ucrtbase), ours as a fallback (the same heap).
typedef void *(__cdecl *AMalloc)(size_t, size_t);
typedef void (__cdecl *AFree)(void *);
static bool list_reserve(char *list, u32 n)
{
    static AMalloc am;
    static AFree af;
    if (n <= *(u32 *)(list + 16)) return true;
    if (!am) {
        HMODULE ucrt = GetModuleHandleA("ucrtbase.dll");
        am = ucrt ? (AMalloc)GetProcAddress(ucrt, "_aligned_malloc") : 0;
        af = ucrt ? (AFree)GetProcAddress(ucrt, "_aligned_free") : 0;
        if (!am || !af) { am = _aligned_malloc; af = _aligned_free; }
    }
    char *p = (char *)am((size_t)n * 24, 16);
    if (!p) return false;
    char *old = *(char **)list;
    if (old) memcpy(p, old, (size_t)*(u32 *)(list + 12) * 24);
    *(u32 *)(list + 16) = n;
    if (old) af(old);
    *(char **)list = p;
    g_stat_grown++;
    return true;
}

// A side's contact list, built as i_visman builds its own player's side's (observers: that side's records).
static Ent g_ents[MAX_ENTRIES];
static u32 collect(u32 side)
{
    u32 count = 0;
    void *tg = *(void **)(g_vm + VM_TARGET);
    u32 rn;
    char *r0 = recs(&rn);
    if (!tg || !r0 || rn > 8192) return 0;
    Contacts contacts = (Contacts)slot(tg, TG_CONTACTS);
    __try {
        for (u32 i = 0; i < rn; i++) {
            char *r = r0 + (size_t)i * VR_SIZE;
            void *u = *(void **)(r + VR_UNIT);
            if (!u || *(u32 *)(r + VR_SIDE) != side || r[VR_NOOBS]) continue;
            if (!(((float (*)(void *))slot(u, U_ALIVE))(u) > 0.0f)) continue;
            u32 index = ((u32 (*)(void *))slot(u, U_TARGET))(u);
            if (index == 0xffffffffu) continue;
            const char *c = contacts(tg, index);
            const char *ce = c ? *(const char *const *)c : 0;
            u32 cn = c ? *(const u32 *)(c + 12) : 0;
            for (u32 k = 0; ce && k < cn; k++) {
                const char *p = *(const char *const *)(ce + (size_t)k * 48 + 24);
                if (!p) continue;
                unsigned short pa = *(const unsigned short *)(p + PAIR_A), pb = *(const unsigned short *)(p + PAIR_B);
                if (!(pa >= 1000 || (pb >= 1000 && !(*(const u32 *)(p + PAIR_FLAGS) & 0xf000)))) continue;
                const void *unit = *(const void *const *)p;
                float fa = pa * 0.001f, fb = pb * 0.001f;
                u32 j = 0;
                while (j < count && g_ents[j].unit != unit) j++;
                if (j < count) { g_ents[j].a = max(g_ents[j].a, fa); g_ents[j].b = max(g_ents[j].b, fb); }
                else if (count < MAX_ENTRIES) { g_ents[count].unit = unit; g_ents[count].a = fa; g_ents[count].b = fb; count++; }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return count;
}

static void build_list(u32 side, std::vector<SightEntry> &out)
{
    out.clear();
    u32 n = collect(side);
    for (u32 i = 0; i < n; i++) {
        u64 key;
        if (session_unit_key((Object *)g_ents[i].unit, &key)) out.push_back({ key, g_ents[i].a, g_ents[i].b });
    }
}

// The side the other player plays, from this game's records: the enemy's in a versus battle, else this player's own.
static u32 other_players_side(bool opposite = false)
{
    u32 rn;
    char *r0 = recs(&rn);
    int flag = session_versus() != opposite ? VR_ENEMY : VR_OWN;
    for (u32 i = 0; r0 && i < rn && i < 8192; i++)
        if (r0[(size_t)i * VR_SIZE + flag]) return *(u32 *)(r0 + (size_t)i * VR_SIZE + VR_SIDE);
    return 0;
}

// Joining game: the host's list in place of this game's side list (only for units this game counts as enemies).
static void put_host_list(void)
{
    std::vector<Ent> ents;
    u32 rn;
    char *r0 = recs(&rn);
    if (!r0 || rn > 8192) return;
    for (u32 i = 0; i < rn; i++) {
        char *r = r0 + (size_t)i * VR_SIZE;
        Object *u = *(Object **)(r + VR_UNIT);
        u64 key;
        if (!r[VR_ENEMY] || !u || !session_unit_key(u, &key)) continue;
        auto it = g_host.find(key);
        if (it == g_host.end() || !(it->second.first >= 1.0f || it->second.second >= 1.0f)) continue;
        ents.push_back({ u, it->second.first, it->second.second });
    }
    std::sort(ents.begin(), ents.end(), [](const Ent &x, const Ent &y) { return (size_t)x.unit < (size_t)y.unit; });
    char *list = g_vm + VM_SIDE_LIST;
    u32 n = (u32)ents.size();
    if (n > *(u32 *)(list + 16) && !list_reserve(list, n)) n = *(u32 *)(list + 16);
    char *d = *(char **)list;
    for (u32 k = 0; d && k < n; k++) {
        memcpy(d + (size_t)k * 24, &ents[k].unit, 8);
        memcpy(d + (size_t)k * 24 + 8, &ents[k].unit, 8);
        memcpy(d + (size_t)k * 24 + 16, &ents[k].a, 4);
        memcpy(d + (size_t)k * 24 + 20, &ents[k].b, 4);
    }
    *(u32 *)(list + 12) = d ? n : 0;
    if (g_bi != BUILD_BETA26) *(u32 *)(g_vm + VM_SEL_LIST + 12) = 0;     // (the 2026 beta only times units by it)
    g_put_seq = g_host_seq;
    g_stat_put++;
    g_stat_entries += n;
}

static void tick_hook(char *vm, float dt)
{
    if (vm == g_vm && joining()) put_host_list();
    g_tick(vm, dt);
}

// The 2026 beta's 0.5 s step, as the game does it (adv_game+0x162420), with the host's list put in after the build.
static u64 g_stat_step_failed;
static void step_body(char *vm, float dt)
{
    char *ag = g_ag;
    float t = dt + *(float *)(vm + VM_TIMER);
    *(float *)(vm + VM_TIMER) = t;
    if (!(t > *(float *)(ag + VM_HALF))) return;
    *(float *)(vm + VM_TIMER) = 0;
    ((VisBuild)(ag + VM_BUILD))(vm, 0);
    put_host_list();
    g_stat_steps++;
    ((VisStep)(ag + VM_STATES))(vm, t);
    void *gs = *(void **)(vm + VM_GAMESPEC);
    char all = ((char (*)(void *))slot(gs, 40))(gs) || ((char (*)(void *))slot(gs, 14))(gs);
    u32 rn;
    char *r = recs(&rn);
    char *end = r + (size_t)VR_SIZE * rn;
    for (; r && r != end; r += VR_SIZE) {
        void *u = *(void **)(r + VR_UNIT);
        if (r[VR_NOOBS] && !((char (*)(void *))slot(u, U_KEEP))(u)) {
            memcpy(r + VR_A, ag + VM_ZERO4, 16);
            r[VR_IDENT] = 0;
            continue;
        }
        if (!((VisShow)(ag + VM_SHOW))(vm, r, all)) continue;
        float secs = *(float *)(ag + VM_LONG);
        if (r[VR_ENEMY] && !r[VR_NOOBS] && !((int (*)(void *))slot(u, U_LONG))(u) && !vm[VM_BLOCK] && !vm[VM_BLOCK + 1] &&
            (unsigned char)(r[VR_STATE] - 1) <= 2 && !vm[VM_BLOCK + 4])
            secs = *(float *)(ag + VM_SHORT);
        ((void (*)(void *, float))slot(u, U_SHOW))(u, secs);
        g_stat_shows++;
    }
}

static void step_hook(char *vm, float dt)
{
    if (vm != g_vm || !joining()) { g_step(vm, dt); return; }
    __try {
        step_body(vm, dt);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_stat_step_failed++;
    }
}

static void install(ObjMan *om)
{
    g_tried = true;
    char v[8];
    if (GetEnvironmentVariableA("MP_SYNC_SIGHT", v, sizeof v) && !atoi(v)) { g_enabled = false; mp_log("sight: each game spots the enemy itself (MP_SYNC_SIGHT=0)"); return; }
    static const unsigned char tick_old[] = { 0x48, 0x8b, 0xc4, 0x53, 0x57, 0x48, 0x83, 0xec, 0x68, 0x83, 0xb9, 0xb4, 0x01, 0x00, 0x00, 0x00 };
    static const unsigned char tick_b26[] = { 0x40, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0x8b, 0xf9, 0x0f, 0x29, 0x74, 0x24, 0x20 };
    static const unsigned char step_b26[] = { 0x40, 0x57, 0x48, 0x83, 0xec, 0x40, 0x0f, 0x29, 0x74, 0x24, 0x30, 0x48, 0x8b, 0xf9, 0x0f, 0x28, 0xf1 };
    const char *agn = "adv_game.codelib";
    g_ag = build_module(agn);
    Object *vm = g_ag ? engine_find(om, "i_visman", "visman") : 0;
    bool b26 = g_bi == BUILD_BETA26;
    char *tick = g_ag ? build_code(agn, VM_TICK, b26 ? tick_b26 : tick_old, b26 ? sizeof tick_b26 : sizeof tick_old) : 0;
    char *step = b26 && g_ag ? build_code(agn, VM_STEP, step_b26, sizeof step_b26) : 0;
    if (!vm || (char *)vm->vtbl != g_ag + VM_VTABLE || !tick || (b26 && (!step || *(float *)(g_ag + VM_HALF) != 0.5f ||
        *(float *)(g_ag + VM_SHORT) != 2.0f || *(float *)(g_ag + VM_LONG) != 10.0f))) {
        mp_log("sight: unknown game build, each game spots the enemy itself");
        return;
    }
    g_vm = (char *)vm;
    g_tick = (VisStep)install_detour(tick, b26 ? sizeof tick_b26 : sizeof tick_old, tick_hook);
    if (b26) g_step = (VisStep)install_detour(step, sizeof step_b26, step_hook);
    if (!g_tick || (b26 && !g_step)) { mp_log("sight: could not be hooked, each game spots the enemy itself"); return; }
    g_ok = true;
    mp_log("sight: what each player sees of the enemy is the host's spotting - its list for the other player's side is sent and put in here");
}

static void send_list(void (*send)(const void *, int), const std::vector<SightEntry> &g_list, unsigned g_list_side)
{
    unsigned parts = (unsigned)((g_list.size() + PER_PACKET - 1) / PER_PACKET);
    if (!parts) parts = 1;
    if (parts > MAX_PARTS) parts = MAX_PARTS;
    unsigned char buf[sizeof(SightHead) + PER_PACKET * sizeof(SightEntry)];
    for (unsigned p = 0; p < parts; p++) {
        size_t from = (size_t)p * PER_PACKET, n = g_list.size() > from ? min((size_t)PER_PACKET, g_list.size() - from) : 0;
        SightHead *h = (SightHead *)buf;
        h->h = make_head(PKT_SIGHT, (int)n);
        h->seq = g_seq;
        h->side = g_list_side;
        h->part = (unsigned short)p;
        h->parts = (unsigned short)parts;
        if (n) memcpy(buf + sizeof(SightHead), &g_list[from], n * sizeof(SightEntry));
        send(buf, (int)(sizeof(SightHead) + n * sizeof(SightEntry)));
    }
    g_sent_at = GetTickCount();
    g_stat_sent++;
}

// Every frame: install; the host builds the other player's side's list after its i_visman rebuilt its own, and sends
// it (and again every second). send_opposite (more players): the extra players on the other side, who get theirs.
void sight_frame(ObjMan *om, void (*send)(const void *, int), void (*send_opposite)(const void *, int))
{
    if (!g_tried && g_enabled && session_role() != ROLE_NONE && !strcmp(g_program, "tacgame")) install(om);
    if (!ready()) return;
    sight_test_step();
    if (session_role() != ROLE_HOST) return;
    float timer = *(float *)(g_vm + VM_TIMER);
    bool rebuilt = timer < g_timer_seen;
    g_timer_seen = timer;
    if (rebuilt && *(int *)(g_vm + VM_BATTLE)) {
        u32 side = other_players_side();
        if (side) {
            std::vector<SightEntry> list;
            build_list(side, list);
            g_list.swap(list);
            g_list_keys.clear();
            for (const SightEntry &e : g_list) g_list_keys[e.key] = true;
            g_list_side = side;
            g_seq++;
            g_stat_built++;
            static int log_lists = -1;
            if (log_lists < 0) { char v[8]; log_lists = GetEnvironmentVariableA("MP_LOG_SIGHT", v, sizeof v) && atoi(v); }
            if (log_lists) {            // (tests: each list as the joining game's "sight" reports it)
                std::vector<u64> keys;
                for (const SightEntry &e : g_list) keys.push_back(e.key);
                std::sort(keys.begin(), keys.end());
                mp_log("sight: list %u - %zu enemies, hash %08x", g_seq, keys.size(), keys.empty() ? 0 : fnv((const unsigned char *)keys.data(), keys.size() * 8));
            }
            send_list(send, g_list, g_list_side);
            u32 side2 = send_opposite ? other_players_side(true) : 0;
            if (side2) {
                std::vector<SightEntry> list2;
                build_list(side2, list2);
                g_list2.swap(list2);
                g_list2_side = side2;
                send_list(send_opposite, g_list2, g_list2_side);
            }
            return;
        }
    }
    if (g_seq && GetTickCount() - g_sent_at >= 1000) {
        send_list(send, g_list, g_list_side);
        if (send_opposite && g_list2_side) send_list(send_opposite, g_list2, g_list2_side);
    }
}

// Host: whether the other player's side has spotted this unit, by the list last sent to it (any level: what the
// joining game may draw comes from that list). -1: not known (no list yet, not this game's place to say).
int sight_other_sees(u64 key)
{
    if (!ready() || session_role() != ROLE_HOST || !g_seq) return -1;
    return g_list_keys.count(key) ? 1 : 0;
}

void sight_on_packet(const unsigned char *data, int size)
{
    if (size < (int)sizeof(SightHead) || !g_ok || session_role() != ROLE_JOIN) return;
    const SightHead *h = (const SightHead *)data;
    unsigned n = h->h.count;
    if (h->parts == 0 || h->parts > MAX_PARTS || h->part >= h->parts || n > PER_PACKET || size < (int)(sizeof(SightHead) + n * sizeof(SightEntry))) return;
    if (h->seq != g_rx_seq) {
        if ((int)(h->seq - g_rx_seq) < 0 && g_have_host) return;        // an older list
        g_rx_seq = h->seq;
        g_rx_parts = h->parts;
        g_rx_mask = 0;
        g_rx.clear();
    }
    if (g_rx_mask & (1u << h->part)) return;
    g_rx_mask |= 1u << h->part;
    const SightEntry *e = (const SightEntry *)(data + sizeof(SightHead));
    g_rx.insert(g_rx.end(), e, e + n);
    if (g_rx_mask != ((g_rx_parts >= 32) ? 0xffffffffu : (1u << g_rx_parts) - 1)) return;
    g_host.clear();
    for (const SightEntry &x : g_rx) g_host[x.key] = { x.a, x.b };
    g_have_host = true;
    g_host_seq = h->seq;
    g_rx_mask = 0xffffffffu;        // (this list is complete: its repeats change nothing)
}

void sight_report(void)
{
    if (g_stat_built || g_stat_sent)
        mp_log("sight: 5 s: the other player's side's list built %llu times (%zu enemies in it now), sent %llu times", g_stat_built, g_list.size(), g_stat_sent);
    if (g_stat_put)
        mp_log("sight: 5 s: the host's list put in %llu times (%.1f enemies on average); %llu steps done here (%llu units shown, %llu failed)%s",
               g_stat_put, (double)g_stat_entries / (double)g_stat_put, g_stat_steps, g_stat_shows, g_stat_step_failed, g_stat_grown ? ", the list grown" : "");
    g_stat_built = g_stat_sent = g_stat_put = g_stat_entries = g_stat_grown = g_stat_steps = g_stat_shows = g_stat_step_failed = 0;
}

// Tests: "sight" - on the host the list last sent for the other player's side, on the joining game its side list now:
// the count and a hash of the units in it (as unit numbers, sorted) with their levels at or above 1.
static volatile LONG g_test_list;
void sight_test_list(void) { InterlockedExchange(&g_test_list, 1); }

void sight_test_step(void)
{
    if (!g_ok || !InterlockedExchange(&g_test_list, 0)) return;
    std::vector<u64> keys;
    if (session_role() == ROLE_HOST) {
        for (const SightEntry &e : g_list) keys.push_back(e.key);
    } else {
        char *list = g_vm + VM_SIDE_LIST;
        char *d = *(char **)list;
        u32 n = *(u32 *)(list + 12);
        for (u32 k = 0; d && k < n && k < MAX_ENTRIES; k++) {
            u64 key;
            if (session_unit_key(*(Object **)(d + (size_t)k * 24), &key)) keys.push_back(key);
        }
    }
    std::sort(keys.begin(), keys.end());
    unsigned h = keys.empty() ? 0 : fnv((const unsigned char *)keys.data(), keys.size() * 8);
    mp_log("test: sight - %zu enemies in the %s list (list %u), hash %08x", keys.size(), session_role() == ROLE_HOST ? "sent" : "side",
           session_role() == ROLE_HOST ? g_seq : g_put_seq, h);
    // (this game's own side: the game's own list against this module's build of it - they must be the same)
    u32 rn, own = 0, other = other_players_side(), obs_own = 0, obs_other = 0;
    char *r0 = recs(&rn);
    for (u32 i = 0; r0 && i < rn && i < 8192; i++) {
        char *r = r0 + (size_t)i * VR_SIZE;
        if (r[VR_OWN] && !own) own = *(u32 *)(r + VR_SIDE);
        if (r[VR_NOOBS]) continue;
        if (r[VR_OWN]) obs_own++;
        else if (*(u32 *)(r + VR_SIDE) == other) obs_other++;
    }
    std::vector<SightEntry> mine;
    if (own) build_list(own, mine);
    char so[5] = {}, sx[5] = {};
    memcpy(so, &own, 4);
    memcpy(sx, &other, 4);
    mp_log("test: sight - this side %s: %u records, the game's list %u, built here %zu; the other side %s: %u records; battle %d", so, obs_own,
           *(u32 *)(g_vm + VM_SIDE_LIST + 12), mine.size(), sx, obs_other, *(int *)(g_vm + VM_BATTLE));
}
