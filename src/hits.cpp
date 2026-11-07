// Hits. Whatever strikes a unit - a shell, a bullet, a fragment, a blast, a collision - ends in one call of
// the unit's own "I was hit" routine, virtual slot 81 (+0x288) of both unit classes (adv_physic):
//     vehicle  +0x43230   marks on the model, the crew's shock, the statistics, and through slot 235
//                         (+0x3a2e0) the damage to the vehicle's modules - each with its own random numbers
//     soldier  +0x66cc0   the wound (+0x67130), his state, his vehicle told, the statistics
//     on_hit(unit, hit, kind, attacker, number, quiet)
//     hit: 0x80 bytes - place, direction, a second vector, the numbers of the strike (calibre, energy,
//          what was left after the armour ...), +0x60 the part struck, +0x68 the ammunition's description,
//          +0x70 a second description, +0x78 the firing unit
// Both games fly every shell, so both called it - each for its own picture of the battle, in which the
// other player's units stand a tenth of a second back, and each with its own random numbers. A tank could
// burn in one game and drive on in the other until its owner's saved state put that right; the wrong flash,
// the crew bailing out and climbing back, were seen in between.
//
// A hit on a unit is therefore an event of the game that owns the unit:
//   - there the routine runs as always, and what went into it is sent to the other game: the 0x68 bytes of
//     plain numbers, the kind, the descriptions and units by their numbers, and the unit's random state;
//   - a hit this game's own flying shell makes on a unit of the other player is dropped;
//   - a hit received is carried out here by the same routine with the owner's values. Hits that follow
//     from it inside (a crew member struck through the armour) reach the routine again and are treated by
//     the same rule: the owner's game sends each of them as an event of its own.
// The records travel on the shots' reliable, ordered channel (ShotRec.kind 7). The unit's saved state still
// follows and has the last word. MP_SYNC_HITS=0 switches this off, MP_LOG_HITS=1 logs every hit.
//
// public_beta-26494568 (read in IDA): a method was inserted before the routine, so it is slot 82 (+0x290) of both
// classes - vehicle +0x646f0 (the module damage through its slot 340, +0x580c0), soldier +0x9d8c0 - with the same
// arguments and the same 0x80-byte hit (+0x60 the part, +0x68 the ammunition, +0x70 the second description). The
// soldier's routine goes on below a life of 0.5 there (it stops below 0.1): a soldier who is down can be hit again.
#include "common.h"
#include "build.h"
#include "protocol.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <vector>
#include <unordered_map>

typedef void (*OnHit)(Object *unit, float *hit, u32 kind, void *attacker, u32 number, char quiet);
static OnHit g_techn_hit, g_human_hit;
static ObjMan *g_om;
static bool g_enabled = true, g_installed, g_tried, g_trace;
static DWORD g_main_thread;
static CRITICAL_SECTION g_lock;
static std::vector<ShotRec> g_from_threads;         // hits of this game's units noted on another thread
static std::vector<ShotRec> g_wait;                 // received, carried out before the next simulation step
static volatile LONG64 g_stat_own, g_stat_dropped, g_stat_applied, g_stat_missing, g_stat_failed, g_stat_other_numbers;

#pragma pack(push, 1)
struct HitRec {                                     // travels in ShotRec.launch; the unit struck is ShotRec.obj2,
    unsigned char numbers[0x68];                    // the attacker .shooter, the firing unit .obj5, the two
    u32 number, seed;                               // descriptions .ammo / .weapon, the kind .arg6
    unsigned char quiet, vehicle, local, pad;      // local: place, direction and normal are in the unit's own frame
};
#pragma pack(pop)
static_assert(sizeof(HitRec) <= sizeof(((ShotRec *)0)->launch), "a hit has to fit into a shot record");

enum { H_AMMO = 0x68, H_DESC2 = 0x70, H_SHOOTER = 0x78, H_SIZE = 0x80 };
// The unit's random state the routine draws from (beta26: the vehicle's module damage +0x580c0 steps +0xb94, the
// soldier's hit +0x9d8c0 +0x3610).
#define SEED_VEHICLE BF(0xb64, 0xb94)
#define SEED_SOLDIER BF(0x2768, 0x3610)

// The hit's place (+0x00), the shell's direction (+0x10) and the surface's normal (+0x20; the vehicle's
// hit routine turns place and normal into a mark, and the direction into an angle). They are in the
// world, and the other game has the unit a little elsewhere (shown about a tenth of a second behind, or
// turned a degree): a hit carried out there with the owner's world numbers struck another part of the
// vehicle - another module damaged, or none (with saved states switched off 7 of 14 vehicles of a
// skirmish fight ended in another state). So they travel in the unit's own frame - rows of its rotation
// at +0xf0, place at +0x1b0 (UNIT_ROT / UNIT_POS; beta26 +0x100 / +0x1c0, i_human::place adv_physic+0xc4770
// writes them there), world = place + l0 * row0 + l1 * row1 + l2 * row2 - and are put back into
// the world by the unit as this game has it.
static void to_local(const char *unit, float *v, bool point)
{
    const float *rot = (const float *)(unit + UNIT_ROT), *pos = (const float *)(unit + UNIT_POS);
    float w[3] = { v[0] - (point ? pos[0] : 0), v[1] - (point ? pos[1] : 0), v[2] - (point ? pos[2] : 0) };
    for (int i = 0; i < 3; i++) v[i] = w[0] * rot[i * 4] + w[1] * rot[i * 4 + 1] + w[2] * rot[i * 4 + 2];
}
static void to_world(const char *unit, float *v, bool point)
{
    const float *rot = (const float *)(unit + UNIT_ROT), *pos = (const float *)(unit + UNIT_POS);
    float w[3];
    for (int j = 0; j < 3; j++) w[j] = v[0] * rot[j] + v[1] * rot[4 + j] + v[2] * rot[8 + j] + (point ? pos[j] : 0);
    memcpy(v, w, sizeof w);
}

static u32 desc_id(void *p)
{
    if (!p || !readable(p, 0x10)) return 0xffffffffu;
    __try { return ((u32 (*)(ObjMan *, void *))g_om->vtbl[OM_SLOT_DESC_ID])(g_om, p); }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return 0xffffffffu; }
}
static void *desc_ptr(u32 id) { return id == 0xffffffffu ? 0 : ((void *(*)(ObjMan *, u32))g_om->vtbl[OM_SLOT_DESC_PTR])(g_om, id); }

static u64 unit_key_of(void *p)
{
    u64 key = 0;
    if (!p || !readable(p, sizeof(Object))) return 0;
    Object *o = (Object *)p;
    if (o->objman != g_om || !readable(o->iface, 8)) return 0;
    session_unit_key(o, &key);
    return key;
}

static bool active(void)
{
    return g_enabled && session_role() != ROLE_NONE && session_connected() && session_simulating() && session_versus_or_coop();
}

static void hit_hook(OnHit engine, bool vehicle, Object *unit, float *hit, u32 kind, void *attacker, u32 number, char quiet)
{
    u64 key;
    if (!active() || !readable(hit, H_SIZE) || !session_unit_key(unit, &key)) { engine(unit, hit, kind, attacker, number, quiet); return; }
    if (!session_owns(unit)) { InterlockedIncrement64(&g_stat_dropped); return; }       // its owner's game says what struck it
    ShotRec s = {};
    HitRec h = {};
    memcpy(h.numbers, hit, sizeof h.numbers);
    if (readable((char *)unit + UNIT_ROT, 0xd0)) {
        to_local((char *)unit, (float *)h.numbers, true);
        to_local((char *)unit, (float *)(h.numbers + 0x10), false);
        to_local((char *)unit, (float *)(h.numbers + 0x20), false);
        h.local = 1;
    }
    h.number = number;
    h.quiet = (unsigned char)quiet;
    h.vehicle = vehicle;
    h.seed = *(u32 *)((char *)unit + (vehicle ? SEED_VEHICLE : SEED_SOLDIER));
    s.kind = 7;
    s.arg6 = kind;
    s.obj2 = key;
    s.shooter = unit_key_of(attacker);
    s.obj5 = unit_key_of(*(void **)((char *)hit + H_SHOOTER));
    s.ammo = desc_id(*(void **)((char *)hit + H_AMMO));
    s.weapon = desc_id(*(void **)((char *)hit + H_DESC2));
    memcpy(s.launch, &h, sizeof h);
    // sent before it is carried out: the hits that follow from it inside come after it there too
    if (GetCurrentThreadId() == g_main_thread) shots_queue(s);
    else { EnterCriticalSection(&g_lock); g_from_threads.push_back(s); LeaveCriticalSection(&g_lock); }
    InterlockedIncrement64(&g_stat_own);
    if (g_trace) mp_log("hit: %s struck (kind %u, part %x, ammunition %u), sent", unit->full, kind, *(u32 *)((char *)hit + 0x60), s.ammo);
    engine(unit, hit, kind, attacker, number, quiet);
}

static void techn_hit_hook(Object *unit, float *hit, u32 kind, void *attacker, u32 number, char quiet) { hit_hook(g_techn_hit, true, unit, hit, kind, attacker, number, quiet); }
static void human_hit_hook(Object *unit, float *hit, u32 kind, void *attacker, u32 number, char quiet) { hit_hook(g_human_hit, false, unit, hit, kind, attacker, number, quiet); }

static bool carry_out(OnHit engine, Object *unit, float *hit, u32 kind, void *attacker, u32 number, char quiet)
{
    __try {
        engine(unit, hit, kind, attacker, number, quiet);
        return true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

static void apply(const ShotRec &s)
{
    HitRec h;
    memcpy(&h, s.launch, sizeof h);
    Object *unit = session_unit(s.obj2);
    if (!unit || session_owns(unit)) { g_stat_missing++; return; }
    bool vehicle = !strcmp(unit->iface, "i_techn");
    if (!readable(unit, vehicle ? SEED_VEHICLE + 0xc : SEED_SOLDIER + 8)) { g_stat_missing++; return; }
    if (vehicle != (h.vehicle != 0)) { g_stat_missing++; return; }
    unsigned char hit[H_SIZE] = {};
    memcpy(hit, h.numbers, sizeof h.numbers);
    if (h.local) {
        to_world((char *)unit, (float *)hit, true);
        to_world((char *)unit, (float *)(hit + 0x10), false);
        to_world((char *)unit, (float *)(hit + 0x20), false);
    }
    void *ammo = desc_ptr(xlate_desc(s.ammo)), *desc2 = desc_ptr(xlate_desc(s.weapon));
    void *shooter = s.obj5 ? session_unit(s.obj5) : 0, *attacker = s.shooter ? session_unit(s.shooter) : 0;
    memcpy(hit + H_AMMO, &ammo, 8);
    memcpy(hit + H_DESC2, &desc2, 8);
    memcpy(hit + H_SHOOTER, &shooter, 8);
    // The part struck is named by a render handle, and the other game may number those differently (an
    // operation battle with a history): carried out with the owner's number it left a mark fixed to a node
    // that does not exist here, and the game crashed when the mark was next drawn (basic_render+0x13193).
    u32 part;
    memcpy(&part, hit + 0x60, 4);
    if (!state_local_handle(unit, &part)) { g_stat_other_numbers++; return; }       // the unit's state will tell
    memcpy(hit + 0x60, &part, 4);
    *(u32 *)((char *)unit + (vehicle ? SEED_VEHICLE : SEED_SOLDIER)) = h.seed;
    if (carry_out(vehicle ? g_techn_hit : g_human_hit, unit, (float *)hit, s.arg6, attacker, h.number, (char)h.quiet)) g_stat_applied++;
    else g_stat_failed++;
    if (g_trace) mp_log("hit: %s struck in its owner's game (kind %u, part %x), carried out here", unit->full, s.arg6, *(u32 *)(hit + 0x60));
}

void hits_received(const ShotRec &s)
{
    if (g_wait.size() < 16384) g_wait.push_back(s);
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

static void install(ObjMan *om)
{
    g_tried = true;
    g_om = om;
    g_main_thread = GetCurrentThreadId();
    char v[16];
    if (GetEnvironmentVariableA("MP_SYNC_HITS", v, sizeof v)) g_enabled = atoi(v) != 0;
    g_trace = GetEnvironmentVariableA("MP_LOG_HITS", v, sizeof v) && atoi(v);
    if (!g_enabled) { mp_log("hit: each game works out every hit for itself (MP_SYNC_HITS=0)"); return; }
    char *base = build_module("adv_physic.codelib");
    if (!base) return;
    InitializeCriticalSection(&g_lock);
    // the classes' tables, their hit slot and what it holds (beta26: i_techn::vftable +0x15bda8 and i_human::vftable
    // +0x156768, read in IDA; their only references besides the tables are the unwind data, no direct calls)
    size_t techn_vt = BV(0xc7790, 0xc7750, 0x15bda8), human_vt = BV(0xc6518, 0xc64d8, 0x156768), slot = BF(81, 82);
    char *techn_fn = base + BV(0x43230, 0x43230, 0x646f0), *human_fn = base + BV(0x66cc0, 0x66c90, 0x9d8c0);
    void **techn = (void **)(base + techn_vt) + slot, **human = (void **)(base + human_vt) + slot;
    if (!techn_vt || !human_vt || !slot || !readable(techn, 8) || !readable(human, 8) || *techn != techn_fn || *human != human_fn) {
        g_enabled = false;
        mp_log("hit: unknown game build, each game works out every hit for itself");
        return;
    }
    g_techn_hit = (OnHit)techn_fn;
    g_human_hit = (OnHit)human_fn;
    g_installed = patch_slot(techn, techn_fn, techn_hit_hook) && patch_slot(human, human_fn, human_hit_hook);
    if (!g_installed) g_enabled = false;
    mp_log("hit: %s", g_installed ? "a hit on a unit is its owner's game's: worked out there, sent, and carried out the same here"
                                  : "could not be hooked, each game works out every hit for itself");
}

// ---------------------------------------------------------------- a soldier's condition (ShotRec.kind 10)
// What became of a soldier is his condition, soldier +0x7c8 (+0x38 of the block at +0x790): the battle's
// result goes by it alone (adv_game+0xf80c0, written to config\tacback_image.cfgpack for the operation: below
// 0.1 killed, below 0.5 wounded, below 0.95 lightly hurt, else unhurt), and so does the soldier's own update,
// which carries out his death or wound the step after it has dropped (his flags +0x7d8 then read as in the
// owner's game). A hit carried out here with the owner's numbers ends the same way only if the soldier was
// in the same state before it; where his saved state cannot be loaded (the two games number his model's
// parts differently - most of the other player's units in an operation battle between two PCs), the two
// drifted apart: a soldier killed in his owner's game was left lightly hurt here, and the host's game,
// which writes the operation's result, brought the other player's dead back into the next battle.
// So the condition is the owner's: his game sends it whenever it changes, and this game puts it into its
// copy of the soldier before every step (and as soon as it arrives, for the end of the battle).
// beta26: the condition is at +0x7f8 (the soldier's hit +0x9d8c0 reads it, slot 189 +0xa9170 "alive" compares it
// with 0.5), his flags at +0x808.
#define S_LIFE BF(0x7c8, 0x7f8)
static std::unordered_map<u64, float> g_cond_sent;          // this game's soldiers: the value last sent
static std::unordered_map<u64, float> g_cond_want;          // the other player's soldiers: their owner's value
static u64 g_stat_cond_sent, g_stat_cond_set;
static DWORD g_cond_scan;

static int cond_band(float c) { return c < 0.1f ? 0 : c < 0.5f ? 1 : c < 0.95f ? 2 : 3; }

static void condition_apply(u64 key, float c)
{
    Object *u = session_unit(key);
    if (!u || session_owns(u) || strcmp(u->iface, "i_human") || !readable(u, S_LIFE + 8)) return;
    float *p = (float *)((char *)u + S_LIFE);
    if (*p != c) { *p = c; g_stat_cond_set++; }
}

void conditions_received(const ShotRec &s)
{
    float c;
    memcpy(&c, s.launch, 4);
    if (!(c == c) || c < -1 || c > 2) return;
    g_cond_want[s.obj2] = c;
    condition_apply(s.obj2, c);
}

void conditions_resend(void) { g_cond_sent.clear(); }        // a new connection: every soldier's once more

static void conditions_owner(ObjMan *om)
{
    DWORD now = GetTickCount();
    if (now - g_cond_scan < 250) return;
    g_cond_scan = now;
    u32 count;
    Object **all = all_objects(om, &count);
    for (u32 i = 0; i < count; i++) {
        Object *o = all[i];
        u64 key;
        if (strcmp(o->iface, "i_human") || !session_unit_key(o, &key) || !session_owns(o) || !readable(o, S_LIFE + 8)) continue;
        float c = *(float *)((char *)o + S_LIFE);
        if (!(c == c)) continue;
        auto it = g_cond_sent.find(key);
        if (it != g_cond_sent.end() && cond_band(it->second) == cond_band(c) && fabsf(it->second - c) < 0.02f) continue;
        g_cond_sent[key] = c;
        ShotRec r = {};
        r.kind = 10;
        r.obj2 = key;
        memcpy(r.launch, &c, 4);
        shots_queue(r);
        g_stat_cond_sent++;
    }
}

// Before a simulation step: hits noted on other threads go out, hits of the other game are carried out.
void hits_sim_step(ObjMan *om)
{
    if (active()) {
        conditions_owner(om);
        for (auto &kv : g_cond_want) condition_apply(kv.first, kv.second);
    }
    if (!g_tried) install(om);
    if (!g_installed) { g_wait.clear(); return; }
    std::vector<ShotRec> late;
    EnterCriticalSection(&g_lock);
    late.swap(g_from_threads);
    LeaveCriticalSection(&g_lock);
    if (active()) for (ShotRec &s : late) shots_queue(s);
    if (g_wait.empty()) return;
    if (!xlate_ready()) return;                 // the other game's description numbers cannot be read yet
    std::vector<ShotRec> now;
    now.swap(g_wait);
    for (const ShotRec &s : now) apply(s);
}

void hits_report(void)
{
    if (g_stat_cond_sent || g_stat_cond_set) {
        mp_log("hit: 5 s: %llu conditions of this game's soldiers sent, %llu of the other player's soldiers set to their owner's", g_stat_cond_sent, g_stat_cond_set);
        g_stat_cond_sent = g_stat_cond_set = 0;
    }
    if (!g_stat_own && !g_stat_dropped && !g_stat_applied && !g_stat_missing && !g_stat_failed && !g_stat_other_numbers) return;
    mp_log("hit: 5 s: %lld on this game's units worked out here and sent, %lld of the other game's carried out here, %lld of this game's own on the other player's units dropped, %lld for units not here, %lld failed, %lld not carried out (the part struck could not be found here)",
           g_stat_own, g_stat_applied, g_stat_dropped, g_stat_missing, g_stat_failed, g_stat_other_numbers);
    g_stat_own = g_stat_dropped = g_stat_applied = g_stat_missing = g_stat_failed = g_stat_other_numbers = 0;
}
