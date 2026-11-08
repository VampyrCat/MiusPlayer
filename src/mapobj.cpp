// The map's objects (mpmod2): buildings, fences, trees, poles - what happens to them is the host's.
//
// i_orender (basic_game) keeps every object of the map (a battle map has some 235,000) in a pool of 272-byte records:
// beta16 / public +792 (count +804), beta26 +880 (+892). A record: +16 its matrix (row 3 the position), +128 the
// orender and +168 the index its hit callback is given, +136 the hit callback (the game's object hit function:
// basic_game beta16 +0xaea00, public +0xae910, beta26 +0xe9ad0 - (orender, index, hit) -> double), +144 its type,
// +152 a body (beta26: a kind 3 object with one breaks like a building), +216 an id (from its place: the same in every
// game), +220 damage level, +221 map cell, +228 scene handle, +232 health, +239 state (8 changed, 0x0c destroyed,
// 0x20 burning). The type: +96 / +104 strength against other hits / blasts, +108 health at the start, +244 kind (1
// bends, 2 building, 4 breaks), +664 destructible, +675 collapses from blasts only. Hits too weak to do damage, and
// the energy left after a collapse, are stored (beta16 / public +2624, count +2636; beta26 +2792 / +2804, 48-byte
// entries, the object's index first) and replayed twice a second through the hit function. The orender's slots 4
// (active(index, on)) and 15 (destroyed(index)); its scene manager (+256) slot 7 places an object's model (handle,
// beta26 also the matrix). (The Mius Framework research, kb/engine/map-damage.md and its mod mius_mp; the pool, the
// hit function and the stored list checked in IDA on beta16 and beta26.)
//
// Each game worked out from its own shells what they did to the objects, so a house could stand in one game and lie
// in ruins in the other - and the joining player's game decides what his soldiers can see. Now the host's game sends
// each damaged object that changed (ShotRec kind 18, two to a record: cell, state, level, id, health, matrix), all of
// them again on a new connection. The joining game hits nothing itself (the hit function is detoured: its own hits
// are dropped) and carries out the host's: a building the host has in ruins gets a hit strong enough to collapse it
// (the game then puts the same ruin in its place), another destroyed object is hidden and marked destroyed, a bent or
// knocked over one gets the host's matrix, and the health the host's.
// Finding what changed on the host: an object the hit function was called for is looked at every 100 ms for 3 s (a
// collapse, a ruin follow the hit a moment later); the whole pool is passed over every 2 s for what changes without a
// hit (a vehicle bending a tree). (Until 2.4 every object's hit callback was patched - a pass over the pool every
// second in the joining game - and the host passed over the pool twice a second: together a tenth of the module's
// time and more.)
// MP_SYNC_OBJECTS=0 switches this off.
#include "common.h"
#include "build.h"
#include "protocol.h"
#include "detour.h"
#include <string.h>
#include <unordered_map>
#include <vector>

#define OR_VTABLE  BV(0xe5110, 0xe5108, 0x158f48)
#define OBJ_HIT_FN BV(0xaea00, 0xae910, 0xeae10)
#define OBJ_BOUNDS BV(0xb5580, 0xb5490, 0xf2110)
#define OR_POOL    BF(792, 880)
#define OR_STORED  BF(2624, 2792)
enum {
    OR_SCENE = 256, OR_ELEM = 272, OR_SLOT_ACTIVE = 4, OR_SLOT_DESTROYED = 15, STORED_SIZE = 48,
    OE_MATRIX = 16, OE_OWNER = 128, OE_HITFN = 136, OE_TYPE = 144, OE_BODY = 152, OE_INDEX = 168, OE_ID = 216, OE_LEVEL = 220,
    OE_CELL = 221, OE_SCENE = 228, OE_HEALTH = 232, OE_STATE = 239,
    OT_STRENGTH = 96, OT_STRENGTH_BLAST = 104, OT_HEALTH0 = 108, OT_KIND = 244, OT_DESTRUCTIBLE = 664, OT_BLAST_ONLY = 675,
    OBJECTS = 18, PER_REC = 2, REC_SIZE = 60, CHUNK = 4096, PASS_MS = 2000, WATCH_MS = 3000, WATCH_EVERY_MS = 100
};
typedef double (*ObjHit)(void *orender, u32 index, void *hit);
typedef void (*Bounds)(void *, void *);
static ObjHit g_hit;
static Bounds g_bounds;
static char *g_orender;
static bool g_tried, g_ok, g_enabled = true, g_applying;
static std::vector<u32> g_sig;                  // host: what was sent of each object (0: undamaged / nothing yet)
static u32 g_scan_pos;
static DWORD g_next_scan, g_next_full, g_next_watch;
// host: objects the hit function was called for (from any thread), and those being watched until a time
static SRWLOCK g_hit_lock = SRWLOCK_INIT;
static std::vector<u32> g_hit_list;
static std::unordered_map<u32, DWORD> g_watch;
static bool g_full;                             // host: this pass sends every damaged object (every 10 s: a building
                                                // more than one damage level ahead goes up one level per update there)
static std::unordered_map<u64, u32> g_index;    // joining game: (id, cell) -> index
static u32 g_index_n;
static DWORD g_index_at;
static std::vector<unsigned char> g_in;         // joining game: the host's objects waiting for a step
static u64 g_stat_hits, g_stat_looked, g_stat_sent, g_stat_rx, g_stat_collapsed, g_stat_hidden, g_stat_moved, g_stat_health, g_stat_unknown, g_stat_dropped, g_stat_stored;

static bool pool(char **base, u32 *n)
{
    if (!g_orender || !readable(g_orender + OR_POOL, 16)) return false;
    *base = *(char **)(g_orender + OR_POOL);
    *n = *(u32 *)(g_orender + OR_POOL + 12);
    return *base && *n && *n < 1000000 && readable(*base, (size_t)*n * OR_ELEM);
}

// (a time that is 0 is due at once: compared as a difference, so that it holds past 24.8 days of the PC running)
static bool due(DWORD now, DWORD at) { return !at || (int)(now - at) >= 0; }

static bool ready(void) { return g_ok && session_connected() && session_versus_or_coop() && session_host_auth(); }
static bool joining(void) { return ready() && session_role() == ROLE_JOIN; }

// The hit function, detoured: the joining game's own hits on the map's objects are dropped (the host's come
// instead); on the host an object hit is watched for what the hit did to it.
static double hit_hook(void *orender, u32 index, void *hit)
{
    if (!g_applying && joining()) { g_stat_dropped++; return 0.0; }
    double r = g_hit(orender, index, hit);
    if (orender == g_orender && !g_applying && session_role() == ROLE_HOST && ready()) {
        AcquireSRWLockExclusive(&g_hit_lock);
        if (g_hit_list.size() < 65536) g_hit_list.push_back(index);
        g_stat_hits++;
        ReleaseSRWLockExclusive(&g_hit_lock);
    }
    return r;
}

static void install(ObjMan *om)
{
    g_tried = true;
    char v[8];
    if (GetEnvironmentVariableA("MP_SYNC_OBJECTS", v, sizeof v) && !atoi(v)) { g_enabled = false; mp_log("objects: each game works out its own map objects (MP_SYNC_OBJECTS=0)"); return; }
    char *bg = build_module("basic_game.codelib");
    Object *o = bg ? engine_find(om, "i_orender", "orender") : 0;
    if (!o || (char *)o->vtbl != bg + OR_VTABLE) { mp_log("objects: unknown game build, each game works out its own map objects"); return; }
    g_orender = (char *)o;
    g_hit = (ObjHit)(bg + OBJ_HIT_FN);
    g_bounds = (Bounds)(bg + OBJ_BOUNDS);
    // its start, the same in the three builds: mov [rsp+8],rbx; mov [rsp+10h],rbp; mov [rsp+18h],rsi; push rdi
    static const unsigned char hsig[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57 };
    char *hitfn = build_code("basic_game.codelib", OBJ_HIT_FN, hsig, sizeof hsig);
    // the pool as this build has it: its objects carry the game's hit function and name this orender and their index
    char *base;
    u32 n, checked = 0, good = 0;
    if (!pool(&base, &n)) { mp_log("objects: the map's objects could not be read, each game works out its own"); return; }
    for (u32 i = 0; i < n && checked < 256; i += n / 256 + 1, checked++) {
        char *e = base + (size_t)i * OR_ELEM;
        good += *(void **)(e + OE_HITFN) == (void *)g_hit && *(char **)(e + OE_OWNER) == g_orender && *(u32 *)(e + OE_INDEX) == i;
    }
    if (good * 10 < checked * 9) { mp_log("objects: the map's objects are not laid out as expected (%u of %u), each game works out its own", good, checked); return; }
    ObjHit original = hitfn ? (ObjHit)install_detour(hitfn, sizeof hsig, hit_hook) : 0;
    if (!original) { mp_log("objects: the hit function could not be hooked, each game works out its own map objects"); return; }
    g_hit = original;
    g_ok = true;
    mp_log("objects: what happens to the map's %u objects is the host's - its damage is sent and made the same here", n);
}

static void send_rec(ShotRec &r, u32 &k)
{
    if (!k) return;
    r.kind = OBJECTS;
    r.arg6 = k;
    shots_queue(r);
    memset(&r, 0, sizeof r);
    k = 0;
}

// Host: one object - sent if it is damaged and has changed since it was sent (every damaged one on a full pass).
static void look_at(const char *base, u32 i, bool full, ShotRec &r, u32 &k)
{
    const unsigned char *o = (const unsigned char *)base + (size_t)i * OR_ELEM;
    const char *type = *(const char *const *)(o + OE_TYPE);
    u32 id = *(const u32 *)(o + OE_ID);
    if (!type || !id) return;
    g_stat_looked++;
    unsigned char st = o[OE_STATE], lv = o[OE_LEVEL];
    float hp = *(const float *)(o + OE_HEALTH);
    if (!(st & 0x2c) && !lv && !(hp < *(const float *)(type + OT_HEALTH0) - 0.01f)) { g_sig[i] = 0; return; }
    u32 sig = fnv(o + OE_STATE, 1) ^ (fnv(o + OE_LEVEL, 1) * 31) ^ (fnv(o + OE_HEALTH, 4) * 961) ^ (fnv(o + OE_MATRIX, 64) * 29791);
    sig |= 1;
    if (sig == g_sig[i] && !full) return;
    g_sig[i] = sig;
    unsigned char *e = r.launch + k * REC_SIZE;
    e[0] = o[OE_CELL];
    e[1] = st;
    e[2] = lv;
    memcpy(e + 4, &id, 4);
    memcpy(e + 8, &hp, 4);
    const float *m = (const float *)(o + OE_MATRIX);
    for (int row = 0; row < 4; row++) memcpy(e + 12 + 12 * row, m + 4 * row, 12);
    g_stat_sent++;
    if (++k == PER_REC) send_rec(r, k);
}

// Host: the objects the hit function was called for since the last step are watched for a while.
static void take_hits(DWORD now, u32 n)
{
    std::vector<u32> hits;
    AcquireSRWLockExclusive(&g_hit_lock);
    hits.swap(g_hit_list);
    ReleaseSRWLockExclusive(&g_hit_lock);
    for (u32 i : hits) if (i < n) g_watch[i] = now + WATCH_MS;
}

// Host: the objects hit lately every 100 ms, and a part of the pool every step - a pass every 2 s.
static void host_scan(DWORD now)
{
    char *base;
    u32 n;
    if (!pool(&base, &n)) return;
    if (g_sig.size() != n) { g_sig.assign(n, 0); g_scan_pos = 0; g_watch.clear(); }
    ShotRec r = {};
    u32 k = 0;
    take_hits(now, n);
    if (!g_watch.empty() && due(now, g_next_watch)) {
        g_next_watch = now + WATCH_EVERY_MS;
        __try {
            for (auto it = g_watch.begin(); it != g_watch.end();) {
                look_at(base, it->first, false, r, k);
                if (due(now, it->second)) it = g_watch.erase(it);
                else ++it;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_watch.clear();
        }
    }
    if (!g_scan_pos && !due(now, g_next_scan)) { send_rec(r, k); return; }
    if (!g_scan_pos) {
        g_next_scan = now + PASS_MS;
        g_full = due(now, g_next_full);
        if (g_full) g_next_full = now + 10000;
    }
    u32 end = g_scan_pos + CHUNK < n ? g_scan_pos + CHUNK : n;
    __try {
        for (u32 i = g_scan_pos; i < end; i++) look_at(base, i, g_full, r, k);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    send_rec(r, k);
    g_scan_pos = end < n ? end : 0;
}

static bool lookup(unsigned char cell, u32 id, u32 *index)
{
    char *base;
    u32 n;
    if (!pool(&base, &n)) return false;
    u64 key = (u64)id << 8 | cell;
    for (int pass = 0; pass < 2; pass++) {
        if (g_index_n == n) {
            auto it = g_index.find(key);
            if (it != g_index.end() && it->second < n && *(u32 *)(base + (size_t)it->second * OR_ELEM + OE_ID) == id &&
                (unsigned char)base[(size_t)it->second * OR_ELEM + OE_CELL] == cell) {
                *index = it->second;
                return true;
            }
        }
        DWORD now = GetTickCount();
        if (pass || (g_index_n == n && now - g_index_at < 1000)) return false;
        g_index.clear();
        g_index.reserve(n);
        g_index_n = n;
        g_index_at = now;
        for (u32 i = 0; i < n; i++) {
            const char *o = base + (size_t)i * OR_ELEM;
            u32 oid = *(const u32 *)(o + OE_ID);
            if (oid) g_index[(u64)oid << 8 | (unsigned char)o[OE_CELL]] = i;
        }
    }
    return false;
}

// The energy of a collapse that the game stored to replay: only the host's states take a building further.
static bool drop_stored(u32 index)
{
    char *list = *(char **)(g_orender + OR_STORED);
    u32 *n = (u32 *)(g_orender + OR_STORED + 12);
    if (!list || *n > 100000 || !readable(list, (size_t)*n * STORED_SIZE)) return false;
    for (u32 i = 0; i < *n; i++)
        if (*(u32 *)(list + (size_t)i * STORED_SIZE) == index) {
            memmove(list + (size_t)i * STORED_SIZE, list + (size_t)(i + 1) * STORED_SIZE, (size_t)(*n - i - 1) * STORED_SIZE);
            --*n;
            return true;
        }
    return false;
}

static void apply_one(const unsigned char *e)
{
    g_stat_rx++;
    char *base;
    u32 n, index;
    u32 id;
    memcpy(&id, e + 4, 4);
    if (!pool(&base, &n) || !lookup(e[0], id, &index)) { g_stat_unknown++; return; }
    unsigned char *o = (unsigned char *)base + (size_t)index * OR_ELEM;
    unsigned char hst = e[1], hlv = e[2];
    float hhp;
    memcpy(&hhp, e + 8, 4);
    __try {
        const unsigned char *type = *(const unsigned char *const *)(o + OE_TYPE);
        if (!type) return;
        unsigned char st = o[OE_STATE], lv = o[OE_LEVEL];
        int kind = *(const int *)(type + OT_KIND);
        bool gone_there = (hst & 0x0c) == 0x0c, gone_here = (st & 0x0c) == 0x0c;
        bool building = kind == 2 || (g_bi == BUILD_BETA26 && kind == 3 && *(void *const *)(o + OE_BODY));
        if (hlv > lv || (gone_there && !gone_here)) {
            if (building) {
                if (!gone_here && type[OT_DESTRUCTIBLE]) {
                    // a hit that takes its health to nothing at once: the game collapses it and puts its ruin in its
                    // place, as in the host's game (no random number is drawn without the blast flag; a building that
                    // only collapses from blasts gets that flag)
                    struct { float energy, f4, f8, f12; void *unit; u32 flags, pad; } h = {};
                    bool blast = type[OT_BLAST_ONLY] != 0;
                    float strength = *(const float *)(type + (blast ? OT_STRENGTH_BLAST : OT_STRENGTH));
                    float hp = *(const float *)(o + OE_HEALTH);
                    float need = (blast ? hp / 0.75f : hp) + 0.1f * strength;
                    h.energy = (need > strength ? need : strength) * 1.05f + 1.0f;
                    h.flags = blast ? 1u : 0u;
                    g_applying = true;
                    g_hit(g_orender, index, &h);
                    g_applying = false;
                    g_stat_collapsed++;
                    g_stat_stored += drop_stored(index);
                }
            } else if (gone_there && !gone_here) {
                void **vt = *(void ***)g_orender;
                ((void (*)(void *, u32, unsigned char))vt[OR_SLOT_ACTIVE])(g_orender, index, 0);
                ((void (*)(void *, u32))vt[OR_SLOT_DESTROYED])(g_orender, index);
                g_stat_hidden++;
            }
            return;
        }
        if ((hst & 0x08) && !gone_there && hlv == lv) {
            float *m = (float *)(o + OE_MATRIX), d = 0;
            for (int row = 0; row < 4; row++)
                for (int c = 0; c < 3; c++) {
                    float x;
                    memcpy(&x, e + 12 + 12 * row + 4 * c, 4);
                    x -= m[4 * row + c];
                    d += x * x;
                }
            if (d > 1e-8f) {
                for (int row = 0; row < 4; row++) memcpy(m + 4 * row, e + 12 + 12 * row, 12);
                void *scene = *(void **)(g_orender + OR_SCENE);
                if (scene) ((void (*)(void *, u32, const float *))(*(void ***)scene)[7])(scene, *(u32 *)(o + OE_SCENE), m);
                o[OE_STATE] |= 8;
                g_bounds(0, o);
                g_stat_moved++;
            } else if (!(st & 0x08)) {
                o[OE_STATE] |= 8;           // (changed there, its place the same: changed here too)
            }
        }
        if (hhp < *(float *)(o + OE_HEALTH) - 0.01f) {
            *(float *)(o + OE_HEALTH) = hhp;
            g_stat_health++;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_applying = false;
    }
}

void mapobj_sim_step(ObjMan *om)
{
    if (!g_tried && g_enabled && session_role() != ROLE_NONE && !strcmp(g_program, "tacgame")) install(om);
    if (!g_ok) return;
    DWORD now = GetTickCount();
    bool on = ready() && session_simulating();
    if (session_role() == ROLE_JOIN) {
        if (on && !g_in.empty()) {
            std::vector<unsigned char> in;
            in.swap(g_in);
            for (size_t at = 0; at + REC_SIZE <= in.size(); at += REC_SIZE) apply_one(&in[at]);
        }
        mapobj_test_step();
    } else if (on) {
        host_scan(now);
        mapobj_test_step();
    }
}

void mapobj_received(const ShotRec &r)
{
    u32 k = r.arg6 > PER_REC ? PER_REC : r.arg6;
    if (g_in.size() < (1u << 22)) g_in.insert(g_in.end(), r.launch, r.launch + (size_t)k * REC_SIZE);
}

// A new connection: every damaged object once more.
void mapobj_reset(void) { std::fill(g_sig.begin(), g_sig.end(), 0u); g_scan_pos = 0; g_next_scan = g_next_full = 0; g_watch.clear(); }

void mapobj_report(void)
{
    if (g_stat_hits || g_stat_looked)
        mp_log("objects: 5 s: the hit function called %llu times for this game's objects, %zu objects watched, %llu looked at", g_stat_hits, g_watch.size(), g_stat_looked);
    g_stat_hits = g_stat_looked = 0;
    if (g_stat_sent || g_stat_rx || g_stat_dropped)
        mp_log("objects: 5 s: %llu sent; here %llu taken from the host (%llu collapsed, %llu hidden, %llu moved, %llu damaged, %llu not "
               "found, %llu stored hits dropped), %llu hits of this game's own left to the host", g_stat_sent, g_stat_rx, g_stat_collapsed,
               g_stat_hidden, g_stat_moved, g_stat_health, g_stat_unknown, g_stat_stored, g_stat_dropped);
    g_stat_sent = g_stat_rx = g_stat_collapsed = g_stat_hidden = g_stat_moved = g_stat_health = g_stat_unknown = g_stat_dropped = g_stat_stored = 0;
}

// ---- tests: "objects" (how many objects are damaged and destroyed here, and a hash of which, their state and level),
// "demolish <n>": the n-th destructible building (by index) collapses through the game's own hit function, as a
// shell would make it. Carried out on the game thread.
static volatile LONG g_test_list, g_test_demolish;
static int g_test_arg;
void mapobj_test_list(void) { InterlockedExchange(&g_test_list, 1); }
void mapobj_test_list_all(void) { InterlockedExchange(&g_test_list, 2); }
void mapobj_test_demolish(int index) { g_test_arg = index; InterlockedExchange(&g_test_demolish, 1); }

// The n-th object that can be destroyed (a building first, on a map without one any), or n (the pool's count).
// (Each type is read once, and only when it is large enough to have those fields.)
static u32 demolish_find(char *base, u32 n, int which)
{
    std::unordered_map<const void *, int> kinds;
    for (int pass = 0; pass < 2; pass++) {
        int at = 0;
        for (u32 i = 0; i < n; i++) {
            const unsigned char *o = (const unsigned char *)base + (size_t)i * OR_ELEM;
            const unsigned char *type = *(const unsigned char *const *)(o + OE_TYPE);
            if (!type || !*(const u32 *)(o + OE_ID) || (o[OE_STATE] & 0x0c) == 0x0c) continue;
            auto it = kinds.find(type);
            int kd = it != kinds.end() ? it->second :
                     (kinds[type] = readable(type, OT_BLAST_ONLY + 1) ? (*(const int *)(type + OT_KIND) & 0xff) | (type[OT_DESTRUCTIBLE] ? 0x100 : 0) : -1);
            if (kd < 0 || !(kd & 0x100) || (pass == 0 && (kd & 0xff) != 2)) continue;
            if (at++ == which) return i;
        }
    }
    return n;
}

static void demolish(u32 i, unsigned char *o)
{
    const unsigned char *type = *(const unsigned char *const *)(o + OE_TYPE);
    struct { float energy, f4, f8, f12; void *unit; u32 flags, pad; } h = {};
    bool blast = type[OT_BLAST_ONLY] != 0;
    float strength = *(const float *)(type + (blast ? OT_STRENGTH_BLAST : OT_STRENGTH));
    float hp = *(const float *)(o + OE_HEALTH), need = (blast ? hp / 0.75f : hp) + 0.1f * strength;
    h.energy = (need > strength ? need : strength) * 1.05f + 1.0f;
    h.flags = blast ? 1u : 0u;
    const float *m = (const float *)(o + OE_MATRIX);
    __try {
        g_applying = true;
        g_hit(g_orender, i, &h);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    g_applying = false;
    mp_log("test: demolish - object %u (id %08x, kind %d) at %.0f %.0f: state %02x level %u health %.0f", i, *(u32 *)(o + OE_ID),
           *(const int *)(type + OT_KIND), m[12], m[14], o[OE_STATE], o[OE_LEVEL], *(float *)(o + OE_HEALTH));
}
void mapobj_test_step(void)
{
    if (!g_test_list && !g_test_demolish) return;
    char *base;
    u32 n;
    if (!pool(&base, &n)) return;
    if (InterlockedExchange(&g_test_demolish, 0)) {
        u32 i = demolish_find(base, n, g_test_arg);
        if (i < n) demolish(i, (unsigned char *)base + (size_t)i * OR_ELEM);
        else mp_log("test: demolish - no destructible object number %d here", g_test_arg);
    }    if (LONG how = InterlockedExchange(&g_test_list, 0)) {
        u32 damaged = 0, destroyed = 0, h = 2166136261u, listed = 0;
        __try {
            for (u32 i = 0; i < n; i++) {
                const unsigned char *o = (const unsigned char *)base + (size_t)i * OR_ELEM;
                const unsigned char *type = *(const unsigned char *const *)(o + OE_TYPE);
                u32 id = *(const u32 *)(o + OE_ID);
                if (!type || !id) continue;
                unsigned char st = o[OE_STATE], lv = o[OE_LEVEL];
                if (!(st & 0x0c) && !lv) continue;
                damaged++;
                if ((st & 0x0c) == 0x0c) destroyed++;
                if (how == 2 && listed++ < 120)
                    mp_log("test: object %u id %08x cell %u state %02x level %u kind %d health %.0f of %.0f destructible %u", i, id, o[OE_CELL], st, lv,
                           *(const int *)(type + OT_KIND), *(const float *)(o + OE_HEALTH), *(const float *)(type + OT_HEALTH0), type[OT_DESTRUCTIBLE]);
                unsigned char rec[7] = { o[OE_CELL], (unsigned char)(st & 0x0c), lv };
                memcpy(rec + 3, &id, 4);
                for (unsigned char b : rec) { h ^= b; h *= 16777619u; }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        mp_log("test: objects - %u of %u map objects damaged here, %u destroyed, hash %08x", damaged, n, destroyed, h);
    }
}
