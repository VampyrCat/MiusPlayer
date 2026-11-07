// Full object state through the engine's own savegame code.
//
// Every simulation object answers the commands "save" and "load" with a message whose +0x10 field points
// at a byte stream (the same code the game uses to save and restore a battle). Units write their plain
// state fields, their dynamic arrays, and references to other objects as stable object ids, so a blob
// saved in one game can be loaded into the same unit of another game running the same battle.
//
//   stream: +0x00 begin, +0x08 end, +0x10 capacity end, +0x18 read position, +0x1c underrun count
#include "common.h"
#include "build.h"
#include "detour.h"
#include <stdio.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

struct Stream { unsigned char *begin, *end, *cap; u32 pos, underrun; };
struct StateMsg { void **vtbl; const char *name; Stream *stream; u64 spare[8]; };

static void *msg_dtor(void *self, u64) { return self; }
static void *g_msg_vtbl[4] = { (void *)msg_dtor, (void *)msg_dtor, (void *)msg_dtor, (void *)msg_dtor };

// The engine grows the stream itself when it runs out of room; a buffer this large is never outgrown,
// so the memory stays ours.
static const size_t k_scratch = 8u << 20;
static unsigned char *g_scratch;

bool state_save(Object *o, std::vector<unsigned char> &out)
{
    Handler save = engine_lookup(o, "save");
    if (!save) return false;
    if (!g_scratch) g_scratch = (unsigned char *)malloc(k_scratch);
    Stream s = { g_scratch, g_scratch, g_scratch + k_scratch, 0, 0 };
    StateMsg m = { g_msg_vtbl, "save", &s };
    save(o, &m);
    if (s.begin != g_scratch) {          // outgrown after all: take the data, keep the engine's buffer alive
        mp_log("state: %s wrote more than %zu bytes", o->full, k_scratch);
        g_scratch = s.begin;
    }
    out.assign(s.begin, s.end);
    return true;
}

// Returns the number of bytes the object asked for beyond the end of the blob (0 = clean load), or -1.
int state_load(Object *o, const unsigned char *data, size_t size)
{
    Handler load = engine_lookup(o, "load");
    if (!load) return -1;
    Stream s = { (unsigned char *)data, (unsigned char *)data + size, (unsigned char *)data + size, 0, 0 };
    StateMsg m = { g_msg_vtbl, "load", &s };
    load(o, &m);
    return (int)s.underrun;
}

// "save" means "write your state to the byte stream" only for simulation objects. Rendering and resource
// objects use the same command name for something else, so anything outside these modules is left alone.
bool state_is_simulation(Object *o)
{
    static HMODULE mods[3];
    static bool init;
    if (!init) {
        init = true;
        mods[0] = (HMODULE)build_module("adv_physic.codelib");
        mods[1] = (HMODULE)build_module("adv_game.codelib");
        mods[2] = (HMODULE)build_module("basic_physic.codelib");
    }
    Handler save = engine_lookup(o, "save");
    HMODULE m = 0;
    if (!save || !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)save, &m)) return false;
    return m && (m == mods[0] || m == mods[1] || m == mods[2]);
}

bool state_is_unit(Object *o)
{
    return !strcmp(o->iface, "i_techn") || !strcmp(o->iface, "i_human");
}

// ---------------------------------------------------------------- render handles stay local
// Every physical object - a unit, and the objects nested inside it - is saved through the engine's base
// routine (adv_physic+0x81c90) and loaded through its counterpart (adv_physic+0x82090). Part of that data
// is the list of attached model parts (array at +0x850, count at +0x85c, 0x60 bytes each), and each part
// is saved together with the renderer's handles for it (scene node at +0x48, model instance at +0x4c).
// Those handles only mean something in the
// process that saved them; drawn through another game's handles the unit crashes the renderer.
// While a state from the other game is being loaded, the base load is therefore wrapped: the object's own
// handles are noted before and put back after. Loading a saved game the normal way is not touched.
//
// public_beta-26494568 has the base routines three times, the same code for three classes: a soldier
// (save +0xdb7f0, load +0xdbaa0, called first by i_human's "save" +0x85390 / "load" +0x85b30), the object a
// soldier carries at +0x2b50 (+0xdad70 / +0xdb020, from the same two later on) and a vehicle (+0xdc0b0 /
// +0xdc310, i_techn's +0x121fc0 / +0x1234d0) - where the 2024 builds call the one routine from all three places.
// All three write the same stream as before (the two field tables of the same sizes, 0x296 bytes, then the
// same lists); the part list moved to +0x880 (count +0x88c, room +0x890), 0x60-byte records with the handles
// at +0x48 / +0x4c as before (the saves write 0x40 bytes, +0x48, +0x4c, +0x50 of each, adv_physic +0xdaf52..).
#define PARTS   BF(0x850, 0x880)
#define PARTS_N BF(0x85c, 0x88c)
typedef void (*BaseLoad)(char *, void *);
static BaseLoad g_base_load[3];
static int g_remote_load;

static void base_load_kept(BaseLoad load, char *o, void *msg)
{
    if (!g_remote_load) { load(o, msg); return; }
    std::vector<u32> handles;
    {
        u32 n = *(u32 *)(o + PARTS_N);
        const char *parts = *(const char **)(o + PARTS);
        if (n && n < 4096 && readable(parts, (size_t)n * 0x60))
            for (u32 i = 0; i < n; i++) {
                handles.push_back(*(const u32 *)(parts + (size_t)i * 0x60 + 0x48));
                handles.push_back(*(const u32 *)(parts + (size_t)i * 0x60 + 0x4c));
            }
    }
    load(o, msg);
    // The parts this game already had keep their handles. Parts the other game has in addition are dropped
    // from the list: there is no local handle to draw them with.
    u32 n = *(u32 *)(o + PARTS_N), had = (u32)handles.size() / 2;
    char *parts = *(char **)(o + PARTS);
    if (n > had) *(u32 *)(o + PARTS_N) = n = had;
    if (n && n < 4096 && readable(parts, (size_t)n * 0x60))
        for (u32 i = 0; i < n; i++) {
            *(u32 *)(parts + (size_t)i * 0x60 + 0x48) = handles[i * 2];
            *(u32 *)(parts + (size_t)i * 0x60 + 0x4c) = handles[i * 2 + 1];
        }
}

static void base_load_hook0(char *o, void *msg) { base_load_kept(g_base_load[0], o, msg); }
static void base_load_hook1(char *o, void *msg) { base_load_kept(g_base_load[1], o, msg); }
static void base_load_hook2(char *o, void *msg) { base_load_kept(g_base_load[2], o, msg); }

// Last line of defence. A handle is a place in the renderer's node tables (group << 10 | node). The
// renderer's node look-up (basic_render+0x13ab0; public_beta-26494568 +0x19ad0, the same (render, group,
// node) -> matrix slot, node tables at +0x240, recursing for the parent; the 2024 look-up's prologue matches
// a look-alike there, +0x18f20, which only marks nodes) follows whatever it is given; with a handle from the
// other game that names no live node here it walks into unmapped memory and the game is gone - seen from
// several places in the unit update, always at this function. With another player's states being loaded
// the look-up is therefore wrapped: an access violation inside it ends the look-up (the caller gets the
// first transform slot) instead of ending the game.
typedef u32 (*NodeLookup)(char *, u64, int);
static NodeLookup g_node_lookup;
static u64 g_bad_lookups;
static char *g_seen_render;              // (tests: the renderer the look-ups were last asked of)

// The game runs with its crash reporter on, which takes an access violation before any __try / __except of
// the module sees it: it writes out\minidump.dmp and ends the game (seen in a friend's game: the module's
// guard was on the stack and the game still went down). So the guards are a vectored handler of the module's
// own, put first, that acts only while one of the guarded renderer calls runs on this thread: it puts the
// thread back where the guard began (the context taken just before the call), with a note that the call
// failed. Every other exception goes on to the game's own handling as before.
static thread_local CONTEXT *t_guard;
static thread_local volatile LONG t_guard_failed;

static LONG CALLBACK guard_handler(EXCEPTION_POINTERS *ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || !t_guard) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT *c = t_guard;
    t_guard = 0;
    t_guard_failed = 1;
    *ep->ContextRecord = *c;
    return EXCEPTION_CONTINUE_EXECUTION;
}

__declspec(noinline) static u32 node_lookup_hook(char *render, u64 group, int node)
{
    g_seen_render = render;
    if (t_guard) return g_node_lookup(render, group, node);       // (inside the matrix guard below)
    CONTEXT c;
    t_guard_failed = 0;
    RtlCaptureContext(&c);
    if (t_guard_failed) {
        t_guard_failed = 0;
        g_bad_lookups++;
        return 0;
    }
    t_guard = &c;
    u32 r = g_node_lookup(render, group, node);
    t_guard = 0;
    return r;
}

// The same for a node's matrix (basic_render+0x13120(render, handle, out 4x4, index); public_beta-26494568
// +0x19160, i_gobjects::world_matrix, the same code): it reads the node
// tables itself before it calls the look-up above. A friend's operation battle crashed there (+0x13193) at the
// end of deployment, when a soldier was seated on a vehicle (adv_physic+0x749b0 asks for the seat's point
// through it). A bad handle now gives the unit matrix instead.
typedef void (*NodeMatrix)(char *, u64, float *, u32);
static NodeMatrix g_node_matrix;

__declspec(noinline) static void node_matrix_hook(char *render, u64 handle, float *out, u32 index)
{
    if (t_guard) { g_node_matrix(render, handle, out, index); return; }
    float *volatile out_v = out;            // (read from the stack after a failed call)
    CONTEXT c;
    t_guard_failed = 0;
    RtlCaptureContext(&c);
    if (t_guard_failed) {
        t_guard_failed = 0;
        g_bad_lookups++;
        float *o = out_v;
        if (readable(o, 64)) {
            memset(o, 0, 64);
            o[0] = o[5] = o[10] = o[15] = 1;
        }
        return;
    }
    t_guard = &c;
    g_node_matrix(render, handle, out, index);
    t_guard = 0;
}

// Tests (command "badnode"): the two guarded calls, through the game's own entry points, with a handle that
// names no node - each must come back instead of ending the game.
void state_test_guard(void)
{
    char *render = build_module("basic_render.codelib");
    if (!render || !g_seen_render || !g_node_lookup || !g_node_matrix) { mp_log("test: render guards not ready"); return; }
    u64 before = g_bad_lookups;
    u32 r = ((NodeLookup)(render + BV(0x13ab0, 0x13ab0, 0x19ad0)))(g_seen_render, 0x3ffff0, 5);
    float m[16];
    ((NodeMatrix)(render + BV(0x13120, 0x13120, 0x19160)))(g_seen_render, 0xfffffc05ull, m, 0);
    mp_log("test: bad render handles - %llu of 2 calls caught (look-up gave %u, matrix [0] %.1f), the game goes on", g_bad_lookups - before, r, m[0]);
}

u64 state_bad_lookups(void) { u64 n = g_bad_lookups; g_bad_lookups = 0; return n; }
bool state_remote_loading(void) { return g_remote_load > 0; }

static void install_part_guard(void)
{
    static const unsigned char sig[] = { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56 };
    static const unsigned char lookup_sig[] = { 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xec, 0x20,
                                                0x4c, 0x8b, 0x89, 0x40, 0x02, 0x00, 0x00 };
    static const unsigned char matrix_sig[] = { 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xec, 0x20,
                                                0x49, 0x8b, 0xf0, 0x48, 0x8b, 0xf9 };
    // public_beta-26494568: the three base loads begin alike - mov [rsp+8],rbx / mov [rsp+10h],rbp /
    // mov [rsp+18h],rsi / push rdi / sub rsp,20h (20 bytes, copied) / mov rbx,[rdx+10h]; the lea r8,[rip+..] after
    // that is not copied. The look-up: mov [rsp+8],rbx / mov [rsp+10h],rsi / push rdi / sub rsp,60h (15 copied) /
    // mov r9,[rcx+240h]. The matrix: mov [rsp+10h],rbx / push rdi / sub rsp,20h / mov eax,edx / mov r11d,r9d
    // (15 copied) / and eax,3ffh / mov rdi,r8.
    static const unsigned char sig26[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57,
                                           0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0x5a, 0x10 };
    static const unsigned char lookup_sig26[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x60,
                                                  0x4c, 0x8b, 0x89, 0x40, 0x02, 0x00, 0x00 };
    static const unsigned char matrix_sig26[] = { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x20, 0x8b, 0xc2, 0x45, 0x8b, 0xd9,
                                                  0x25, 0xff, 0x03, 0x00, 0x00, 0x49, 0x8b, 0xf8 };
    static bool tried;
    if (tried) return;
    tried = true;
    bool b26 = g_bi == BUILD_BETA26;
    // the 2024 builds: one base load for every physical object; beta26: the soldier's, his carried object's, the vehicle's
    const size_t loads[3] = { (size_t)BV(0x82090, 0x82020, 0xdbaa0), (size_t)BV(NP, NP, 0xdb020), (size_t)BV(NP, NP, 0xdc310) };
    void (*const hooks[3])(char *, void *) = { base_load_hook0, base_load_hook1, base_load_hook2 };
    char *load[3] = {};
    int found = 0, wanted = 0;
    for (int k = 0; k < 3; k++) {
        if (!loads[k]) continue;
        wanted++;
        load[k] = build_code("adv_physic.codelib", loads[k], b26 ? sig26 : sig, b26 ? sizeof sig26 : sizeof sig);
        if (load[k]) found++;
    }
    if (!found || found != wanted) { mp_log("state: unknown game build, render handles are not guarded"); return; }
    for (int k = 0; k < 3; k++)
        if (load[k]) g_base_load[k] = (BaseLoad)install_detour(load[k], b26 ? 20 : 14, (void *)hooks[k]);
    if (char *lookup = build_code("basic_render.codelib", BV(0x13ab0, 0x13ab0, 0x19ad0), b26 ? lookup_sig26 : lookup_sig,
                                  b26 ? sizeof lookup_sig26 : sizeof lookup_sig))
        g_node_lookup = (NodeLookup)install_detour(lookup, 15, node_lookup_hook);
    if (char *matrix = build_code("basic_render.codelib", BV(0x13120, 0x13120, 0x19160), b26 ? matrix_sig26 : matrix_sig,
                                  b26 ? sizeof matrix_sig26 : sizeof matrix_sig))
        g_node_matrix = (NodeMatrix)install_detour(matrix, 15, node_matrix_hook);
    if (g_node_lookup || g_node_matrix) AddVectoredExceptionHandler(1, guard_handler);
    mp_log("state: render handles %s%s%s", g_base_load[0] ? "stay local when states are loaded" : "could not be guarded",
           g_node_lookup ? ", node look-ups are guarded" : "", g_node_matrix ? ", node matrices too" : "");
}

// Is a state saved by the other game safe to load into this unit? The saved state carries render handles
// in more places than the part list guarded above, and a unit drawn through another process's handles
// crashes the renderer. Two games that built their scene in the same order hand out the same handles; once
// they differ for a unit, its state is not loaded (the unit is then followed by position only).
// The test compares the part list inside the blob with the unit's own. Layout of the base save
// (adv_physic+0x81c90): 0x296 fixed bytes, then counted lists - {u32 capacity, u32 count, records}:
// 17-byte records, 8-byte records, two object ids, u32 records, 0x40-byte records, and the parts
// (0x40 matrix, node handle, instance handle, flags).
// public_beta-26494568 writes the same (+0xdb7f0, +0xdad70, +0xdc0b0: field tables of 0x1ff and 0x93 bytes with
// the description number between them, then the 17- and 8-byte lists and an object id (+0xda6f0), an object id,
// the u32 list, the 0x40 list and the parts), so the blob is read alike; only the object's part list moved.
// How the other game numbers a unit's handles. A handle is group << 10 | node. Two games that built their
// scene in the same order hand out the same numbers; an operation battle with a history does not - only
// the host's battle belongs to the operation, its scene has more groups (14 in a Peleshi battle: every
// unit's handles are 14 groups higher there, the node within the group the same). From the part list of a
// unit's received state against the unit's own, the groups are paired: the other game's -> this game's.
struct GroupMap { u32 remote[6], local[6]; int n; bool ok, same; };
static std::unordered_map<Object *, GroupMap> g_group_maps;

bool state_handles_match(Object *o, const unsigned char *data, size_t size)
{
    GroupMap map = {};
    size_t pos = 0x296;
    auto list = [&](size_t record) -> long long {
        if (pos + 8 > size) return -1;
        u32 n;
        memcpy(&n, data + pos + 4, 4);
        pos += 8;
        if (n > 0x10000 || pos + (size_t)n * record > size) return -1;
        size_t at = pos;
        pos += (size_t)n * record;
        return (long long)at | ((long long)n << 40);
    };
    if (list(17) < 0 || list(8) < 0) return false;
    pos += 8;
    if (list(4) < 0 || list(0x40) < 0) return false;
    long long parts = list(0x4c);
    if (parts < 0) return false;
    u32 n = (u32)(parts >> 40);
    const unsigned char *rec = data + (size_t)(parts & 0xffffffffffll);
    const char *mine = *(const char **)((char *)o + PARTS);
    if (n != *(u32 *)((char *)o + PARTS_N)) { g_group_maps[o] = map; return false; }
    if (n && !readable(mine, (size_t)n * 0x60)) return false;
    bool equal = true;
    map.ok = true;
    for (u32 i = 0; i < n; i++) {
        u32 theirs, here;
        memcpy(&theirs, rec + (size_t)i * 0x4c + 0x40, 4);
        memcpy(&here, mine + (size_t)i * 0x60 + 0x48, 4);
        if (memcmp(rec + (size_t)i * 0x4c + 0x40, mine + (size_t)i * 0x60 + 0x48, 8)) equal = false;
        if ((theirs & 0x3ff) != (here & 0x3ff)) map.ok = false;         // another node: not a matter of numbering
        int k = 0;
        while (k < map.n && map.remote[k] != theirs >> 10) k++;
        if (k < map.n) { if (map.local[k] != here >> 10) map.ok = false; }
        else if (map.n < 6) { map.remote[map.n] = theirs >> 10; map.local[map.n] = here >> 10; map.n++; }
        else map.ok = false;
    }
    map.same = equal;
    g_group_maps[o] = map;
    return equal;
}

// A handle of this unit in the other game's numbering, made this game's. False: it cannot be told which
// node of this game is meant.
bool state_local_handle(Object *o, u32 *handle)
{
    u32 v = *handle;
    if (v == 0xffffffffu) return true;
    auto it = g_group_maps.find(o);
    if (it != g_group_maps.end() && it->second.n) {
        const GroupMap &m = it->second;
        if (!m.ok) return false;
        for (int k = 0; k < m.n; k++)
            if (m.remote[k] == v >> 10) { *handle = m.local[k] << 10 | (v & 0x3ff); return true; }
        return false;
    }
    // no state of this unit has come yet: the handle is taken as it is if it names one of the unit's own groups
    u32 n = *(u32 *)((char *)o + PARTS_N);
    const char *mine = *(const char **)((char *)o + PARTS);
    if (!n || n > 4096 || !readable(mine, (size_t)n * 0x60)) return false;
    for (u32 i = 0; i < n; i++)
        if (*(const u32 *)(mine + (size_t)i * 0x60 + 0x48) >> 10 == v >> 10) return true;
    return false;
}

// ---------------------------------------------------------------- a state from a game that numbers handles differently
// With the groups paired (GroupMap above), a state of the other game can be loaded all the same: what it
// brings in handles is rewritten to this game's numbers afterwards. Handles sit in the unit itself and in
// the lists that hang off it - found by comparing two games with different numbering (tools\handle_fields.py,
// handle_lists.py): a vehicle has them in two dozen lists (weapons, crew places, parts with hit points,
// wheels ...), a soldier in twelve places of his own and one list. The unit's memory and the whole memory
// block of each of those lists are gone through, and every 32-bit value whose group is one of the unit's
// groups in the other game's numbering gets this game's group instead (the node stays).
// That is only safe while no group number means one thing there and another here for the same unit, so a
// unit whose two sets of groups overlap is left alone (followed by position only, as before).
// EXPERIMENTAL and off unless MP_STATE_TRANSLATE=1: it is not complete. Handles also sit in lists inside the
// engine's trees (a vehicle's lights at +0x1b48, its modules at +0x1138), which are not gone through - with a
// state loaded this way a joined game crashed in the lights' update (adv_physic+0xa1bde) one second into a
// Peleshi battle. Without it such a unit is followed by position, animation and hits, as before.
// Not ported to public_beta-26494568: its lists (translate_unit) were found by comparing two 2024 games and
// have not been mapped there (both classes grew, i_techn by about 0x1000 bytes), so it stays off on that build.
static u64 g_stat_translated_units, g_stat_translated_values;

static bool translate_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        char v[8];
        on = GetEnvironmentVariableA("MP_STATE_TRANSLATE", v, sizeof v) ? atoi(v) != 0 : 0;
        if (on && !BF(1, NP)) {
            mp_log("state: MP_STATE_TRANSLATE is not ported to this build, states of the other game's numbering are not loaded");
            on = 0;
        }
    }
    return on != 0;
}

bool state_handles_translatable(Object *o)
{
    if (!translate_enabled()) return false;
    auto it = g_group_maps.find(o);
    if (it == g_group_maps.end()) return false;
    const GroupMap &m = it->second;
    if (!m.ok || m.same || !m.n) return false;
    for (int a = 0; a < m.n; a++)
        for (int b = 0; b < m.n; b++)
            if (m.remote[a] == m.local[b]) return false;
    return true;
}

static u32 translate_block(const GroupMap &m, char *p, size_t bytes)
{
    u32 lo = m.remote[0], hi = m.remote[0], changed = 0;
    for (int k = 1; k < m.n; k++) { if (m.remote[k] < lo) lo = m.remote[k]; if (m.remote[k] > hi) hi = m.remote[k]; }
    for (size_t i = 0; i + 4 <= bytes; i += 4) {
        u32 v;
        memcpy(&v, p + i, 4);
        u32 g = v >> 10;
        if (g < lo || g > hi) continue;
        for (int k = 0; k < m.n; k++)
            if (m.remote[k] == g) {
                v = m.local[k] << 10 | (v & 0x3ff);
                memcpy(p + i, &v, 4);
                changed++;
                break;
            }
    }
    return changed;
}

// The size of the memory block one of the engine's lists lives in (the engine allocates them with
// _aligned_malloc(.., 16)).
static size_t list_bytes(void *p)
{
    __try {
        size_t n = _aligned_msize(p, 16, 0);
        return n <= (1u << 20) ? n : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static void translate_unit(Object *o, const GroupMap &m, bool techn)
{
    if (!BF(1, NP)) return;         // 2024 builds only (see translate_enabled)
    static const unsigned short techn_lists[] = { 0x498, 0x758, 0x820, 0x838, 0xbd8, 0xc00, 0xc28, 0xc50, 0xe80, 0x1158, 0x11e8, 0x1298, 0x1470,
                                                  0x14c8, 0x1600, 0x1838, 0x1868, 0x18a0, 0x18b8, 0x18d0, 0x18e8, 0x1900, 0x1db8, 0x1e98 };
    static const unsigned short human_lists[] = { 0x27a0 };
    char *u = (char *)o;
    size_t size = techn ? 0x1f00 : 0x27b8;
    if (!readable(u, size)) return;
    u32 changed = translate_block(m, u + 0xf0, size - 0xf0);
    const unsigned short *lists = techn ? techn_lists : human_lists;
    int count = techn ? _countof(techn_lists) : _countof(human_lists);
    for (int i = 0; i < count; i++) {
        void *p = *(void **)(u + lists[i]);
        u32 n = *(u32 *)(u + lists[i] + 0xc), room = *(u32 *)(u + lists[i] + 0x10);
        if (!p || ((uintptr_t)p & 0xf) || n > room || !room || room > 65536 || !readable(p, 16)) continue;
        size_t bytes = list_bytes(p);
        if (!bytes || !readable(p, bytes)) continue;
        changed += translate_block(m, (char *)p, bytes);
    }
    if (techn) {
        char *p = *(char **)(u + 0xdf0);            // not a list: one structure, handles at +0x200
        if (p && readable(p + 0x200, 0x10)) changed += translate_block(m, p + 0x200, 0xc);
    }
    g_stat_translated_units++;
    g_stat_translated_values += changed;
}

void state_translate_report(void)
{
    if (!g_stat_translated_units) return;
    mp_log("state: 5 s: %llu states came from the other game's handle numbering: %llu handles rewritten to this game's",
           g_stat_translated_units, g_stat_translated_values);
    g_stat_translated_units = g_stat_translated_values = 0;
}

// Loads a blob into a unit but keeps the unit's current place and motion. Position is handled by the
// smooth transform stream; a state blob is always a little older and would pull the unit back.
//
// keep_view: also keep the fields that describe the unit from this game's player's point of view. The
// engine saves them with the unit (base unit save, adv_physic+0x81c90, field tables at +0xd7e90/+0xd7fc0):
//   +0x6f0  the number of the newest order the unit has had (unit vtable +0x2b0 / +0x2b8, set by the order call)
//   +0x6b0, +0x6b4, +0x6e8, +0x6ec, +0x714  its place in the player's own force (-1 / 0 for the
//           opponent's units)
//   +0x72c  the "seen" countdown, in seconds: the unit is drawn while it is above zero. The game's
//           visibility manager (i_visman) sets it to 10 for the player's own units every 7.5 seconds and
//           for an opponent's unit when one of the player's units has spotted it; the unit counts it down.
//   +0x745, +0x749  flags that go with it (set on hidden units of the opponent)
//   soldier +0x228c, vehicle +0x1e66  "is drawn", worked out from the countdown by the unit's own update -
//           not on every step, so an owner's "drawn" loaded into a hidden unit showed it for a moment
//           after each load: units in the fog of war flickered
// In a versus game the two players see these the other way round. A unit loaded with its owner's values
// is one its owner always sees: here it would be shown for another ten seconds after every load - that is,
// always, spotted or not.
//
// public_beta-26494568 (adv_physic; the offsets of the 2024 builds in brackets):
//   the view fields are 0x30 further on: +0x6e0, +0x6e4, +0x718, +0x71c, +0x720, +0x744, +0x75c, bytes +0x775,
//     +0x779 - the base save's second field table (+0x178360 / +0x1785c0 / +0x178490 against +0xd7fc0) lists
//     the same 39 fields in the same order with the same sizes, each 0x30 further on;
//   soldier: "is drawn" +0x2a58 (+0x228c), the animation instance and second handle +0x21c0 / +0x21c4
//     (+0x1d60 / +0x1d64): i_human's own field table (vtable slot 1: +0x14f770, 475 fields; 2024 +0xd6b50, 471)
//     has them in the same places of its order, and +0x2a58 is set, tested and written by the same functions
//     as +0x228c is in the 2024 build (the constructor's 0x10101 store, a 0x101 store, unit slots 4, 5 and 17);
//   vehicle: "is drawn" +0x2efe (+0x1e66), from i_techn's field table (slot 1: +0x15fb50, 514 fields; 2024
//     +0xd8890) where the bytes around it are all 0x1098 further on; +0xe1d00 sets it and shows or hides the
//     vehicle's models by it.
// The vehicle's lists below come from its save (+0x121fc0; 2024 +0xa6f90), which writes them in the same
// order as before.
struct ViewFields { unsigned short view[7], view_bytes[2], human_drawn, techn_drawn, anim; };
static const ViewFields k_view_old = { { 0x6b0, 0x6b4, 0x6e8, 0x6ec, 0x6f0, 0x714, 0x72c }, { 0x745, 0x749 }, 0x228c, 0x1e66, 0x1d60 };
static const ViewFields k_view_b26 = { { 0x6e0, 0x6e4, 0x718, 0x71c, 0x720, 0x744, 0x75c }, { 0x775, 0x779 }, 0x2a58, 0x2efe, 0x21c0 };
struct Fitted { unsigned short list, count, size, keep; };
struct TechnLists {
    unsigned short points, points_n, points_room;       // model points, 0x20-byte records, node handle at +0x10
    unsigned short seats, seats_n, seats_room, seat_size;
    Fitted fitted[3];
};
// The points: 32-byte records saved raw (+0x17b8, 32 * count from +0x17c4), their +0x10 read as a node handle
// (masked 0x3ff, compared with the heads' handles) by +0xf83e0. The seats: +0x1e40 / +0x1e4c / +0x1e50, records
// of 0x170 bytes saved by +0x1212e0, whose first part (+0x120090) writes +0x00..+0x89 exactly as the 2024 one
// does (+0x4c, +0x50, +0x58 among them, nothing new), and the seat a soldier sits in (vehicle vtable +0x138, as
// before) gives +0x58 to the renderer's node matrix in the soldier's slot 17 (+0xc4770), as +0x749b0 did.
// The fittings: +0x1b30 / +0x1b3c (0x120, the description pointers at +0 and +8 written as numbers through
// object manager slot 49) and +0x1b48 / +0x1b54 (0x1d0, the same record written by +0x83c70 as by +0x51b90 -
// NB in both builds it begins with a 0x40-byte matrix, its description pointers are +0x50 and +0x90; the
// 8 bytes kept below are the 2024 code's choice, left as they are);
// the crates: +0x1a68 / +0x1a74, records now 0x40 bytes ({u32, 4 bytes, u32, u32, u32, byte +0x20} written,
// where the 0x28-byte record had {u32, 4 bytes, u32, float, float, byte +0x20}); the reload code (+0x477d0)
// uses +0x1a68 and +0x1b30 together, as +0x323a0 used +0x1530 and +0x15e8. Each list's room is its count + 4.
static const TechnLists k_techn_old = { 0x1298, 0x12a4, 0x12a8, 0x1900, 0x190c, 0x1910, 0x140,
                                        { { 0x15e8, 0x15f4, 0x120, 8 }, { 0x1600, 0x160c, 0x1d0, 8 }, { 0x1530, 0x153c, 0x28, 0x28 } } };
static const TechnLists k_techn_b26 = { 0x17b8, 0x17c4, 0x17c8, 0x1e40, 0x1e4c, 0x1e50, 0x170,
                                        { { 0x1b30, 0x1b3c, 0x120, 8 }, { 0x1b48, 0x1b54, 0x1d0, 8 }, { 0x1a68, 0x1a74, 0x40, 0x40 } } };

int state_load_keep_motion(Object *o, const unsigned char *data, size_t size, bool keep_view)
{
    const ViewFields &vf = g_bi == BUILD_BETA26 ? k_view_b26 : k_view_old;
    const TechnLists &tl = g_bi == BUILD_BETA26 ? k_techn_b26 : k_techn_old;
    const unsigned short *view = vf.view, *view_bytes = vf.view_bytes;
    const int n_view = _countof(vf.view), n_view_bytes = _countof(vf.view_bytes);
    unsigned char body[UNIT_MOTION_SIZE], kept_bytes[_countof(vf.view_bytes)], kept_drawn;
    const unsigned short drawn = !strcmp(o->iface, "i_human") ? vf.human_drawn : vf.techn_drawn;
    u32 kept[_countof(vf.view)];
    memcpy(body, (char *)o + UNIT_ROT, sizeof body);          // +0xf0 .. +0x2c0 (beta26 +0x100 .. +0x2d0): the whole rigid body
    for (int i = 0; i < n_view; i++) memcpy(&kept[i], (char *)o + view[i], 4);
    for (int i = 0; i < n_view_bytes; i++) kept_bytes[i] = ((unsigned char *)o)[view_bytes[i]];
    kept_drawn = ((unsigned char *)o)[drawn];

    // a soldier's animation instance (+0x1d60) and its second render handle (+0x1d64) are process-local too
    bool human = !strcmp(o->iface, "i_human");
    u32 anim[2];
    memcpy(anim, (char *)o + vf.anim, 8);

    // A vehicle keeps a list of points fixed to nodes of its model (+0x1298: 0x20-byte records, the node's
    // render handle at +0x10; count at +0x12a4, room at +0x12a8). It is made when the model is, and saved
    // raw with the vehicle - handles and all. This game's handles are the only ones that mean anything here.
    bool techn = !strcmp(o->iface, "i_techn");
    std::vector<unsigned char> points;
    if (techn) {
        u32 n = *(u32 *)((char *)o + tl.points_n);
        const unsigned char *p = *(const unsigned char **)((char *)o + tl.points);
        if (n && n < 4096 && readable(p, (size_t)n * 0x20)) points.assign(p, p + (size_t)n * 0x20);
    }
    // A vehicle's seats (+0x1900: records of 0x140 bytes, count at +0x190c; vehicle vtable +0x138 hands one out)
    // carry the render handles of the nodes the seats hang on (+0x4c, +0x58, +0x5c), saved raw with the vehicle.
    // A seated soldier's update (adv_physic+0x4d240) asks the renderer for his seat node's matrix through them and
    // reads what it gets back: with the other game's handle that was no matrix - a friend's game (two PCs)
    // crashed there 17 s into a battle, when the first states arrived. This game's handles stay.
    static const unsigned short k_seat_handles[] = { 0x4c, 0x58, 0x5c };
    const size_t seat = tl.seat_size;
    std::vector<unsigned char> seats;
    if (techn) {
        u32 n = *(u32 *)((char *)o + tl.seats_n);
        const unsigned char *p = *(const unsigned char **)((char *)o + tl.seats);
        if (n && n < 256 && readable(p, (size_t)n * seat)) seats.assign(p, p + (size_t)n * seat);
    }
    // Two more lists of a vehicle start every record with a pointer to a description (+0x15e8: records of
    // 0x120 bytes, count at +0x15f4; +0x1600: records of 0x1d0 bytes, count at +0x160c). They are written
    // as description numbers, and a number of the other game does not always name the same description
    // here; the vehicle update then reads a "name" out of something that is not one (the crash at
    // adv_game+0xd9917 that kept state replication out of operation battles). What a vehicle is fitted
    // with does not change during a battle, so this game's own pointers are kept.
    //
    // A third list (+0x1530: records of 0x28 bytes, count at +0x153c) is the vehicle's ammunition crates.
    // Each record starts with the crate's number in the game's crate store (adv_game, a 48 x 48 grid of
    // cells; number = cell << 16 | place in the cell) and is saved as that number. The store is each
    // game's own: the other game's number names another crate here, or none - and the vehicle's reload
    // code (adv_physic+0x323a0) writes into whatever the store hands back for it, then copies a whole
    // crate record over it. That is what wrecked the store and ended in the crash above. The crates a
    // vehicle has stay this game's.
    const Fitted *k_fitted = tl.fitted;
    const int k_lists = _countof(tl.fitted);
    std::vector<unsigned char> fitted[_countof(tl.fitted)];
    if (techn)
        for (int k = 0; k < k_lists; k++) {
            u32 n = *(u32 *)((char *)o + k_fitted[k].count);
            const unsigned char *p = *(const unsigned char **)((char *)o + k_fitted[k].list);
            if (n && n < 1024 && readable(p, (size_t)n * k_fitted[k].size)) fitted[k].assign(p, p + (size_t)n * k_fitted[k].size);
        }

    install_part_guard();
    // beta26: without the guard the parts' handles could not be kept, so no state of the other game is loaded
    // (the 2024 builds always have it: their routine is checked byte for byte like everything else)
    if (g_bi == BUILD_BETA26 && !g_base_load[0]) return -1;
    g_remote_load++;
    int r = state_load(o, data, size);
    g_remote_load--;
    if (human) memcpy((char *)o + vf.anim, anim, 8);
    if (techn)
        for (int k = 0; k < k_lists; k++) {
            u32 n = *(u32 *)((char *)o + k_fitted[k].count), room = *(u32 *)((char *)o + k_fitted[k].count + 4);
            u32 had = (u32)(fitted[k].size() / k_fitted[k].size);
            unsigned char *p = *(unsigned char **)((char *)o + k_fitted[k].list);
            if (!had) {
                if (n) *(u32 *)((char *)o + k_fitted[k].count) = 0;      // this game's vehicle has none: nor has it now
            } else if (n == had && readable(p, fitted[k].size())) {
                // same fittings: their state is the owner's, the descriptions they point at are this game's
                for (u32 i = 0; i < n; i++) memcpy(p + (size_t)i * k_fitted[k].size, fitted[k].data() + (size_t)i * k_fitted[k].size, k_fitted[k].keep);
            } else if (room >= had && readable(p, fitted[k].size())) {
                memcpy(p, fitted[k].data(), fitted[k].size());           // another set over there: this game's stays whole
                *(u32 *)((char *)o + k_fitted[k].count) = had;
            } else if (k_fitted[k].keep == k_fitted[k].size) {
                *(u32 *)((char *)o + k_fitted[k].count) = 0;             // no room for this game's own: rather none than foreign ones
            }
        }
    if (techn && !points.empty()) {
        u32 n = *(u32 *)((char *)o + tl.points_n), room = *(u32 *)((char *)o + tl.points_room), had = (u32)(points.size() / 0x20);
        unsigned char *p = *(unsigned char **)((char *)o + tl.points);
        if (n == had && readable(p, points.size())) {
            for (u32 i = 0; i < n; i++) memcpy(p + (size_t)i * 0x20 + 0x10, points.data() + (size_t)i * 0x20 + 0x10, 4);
        } else if (room >= had && readable(p, points.size())) {
            // the other game has another model for this vehicle by now (a wreck, say): this game's list stays
            memcpy(p, points.data(), points.size());
            *(u32 *)((char *)o + tl.points_n) = had;
        }
    }

    if (techn && !seats.empty()) {
        u32 n = *(u32 *)((char *)o + tl.seats_n), had = (u32)(seats.size() / seat);
        unsigned char *p = *(unsigned char **)((char *)o + tl.seats);
        static u64 changed_total;
        if (n == had && readable(p, seats.size())) {
            for (u32 i = 0; i < n; i++)
                for (unsigned short f : k_seat_handles) {
                    unsigned char *at = p + (size_t)i * seat + f;
                    const unsigned char *mine = seats.data() + (size_t)i * seat + f;
                    if (memcmp(at, mine, 4)) { memcpy(at, mine, 4); if (changed_total++ < 5) mp_log("state: a seat's render handle from the other game put back (%s seat %u, +0x%x)", o->full, i, f); }
                }
        } else if (n != had && readable(p, seats.size()) && *(u32 *)((char *)o + tl.seats_room) >= had) {
            memcpy(p, seats.data(), seats.size());         // another set of seats over there: this game's stays whole
            *(u32 *)((char *)o + tl.seats_n) = had;
            if (changed_total++ < 5) mp_log("state: %s came with %u seats instead of %u - this game's kept", o->full, n, had);
        }
    }

    // (the handles the load brought in the other game's numbering - after the lists above are this game's again)
    {
        auto it = g_group_maps.find(o);
        if (it != g_group_maps.end() && it->second.ok && !it->second.same && state_handles_translatable(o)) translate_unit(o, it->second, techn);
    }
    memcpy((char *)o + UNIT_ROT, body, sizeof body);
    if (keep_view) {
        for (int i = 0; i < n_view; i++) memcpy((char *)o + view[i], &kept[i], 4);
        for (int i = 0; i < n_view_bytes; i++) ((unsigned char *)o)[view_bytes[i]] = kept_bytes[i];
        ((unsigned char *)o)[drawn] = kept_drawn;
    }
    return r;
}

// ---------------------------------------------------------------- debug: survey and self round trip
// MP_STATE=survey      once, 20 simulated seconds in: size of every object's saved state, by class
// MP_STATE=roundtrip   twice a second: save every unit and load it straight back (tests loading in place)
void state_debug_step(ObjMan *om, u64 sim_ms)
{
    static int mode = -1;
    static u64 last_ms;
    static bool surveyed;
    if (mode < 0) {
        char v[32] = "";
        GetEnvironmentVariableA("MP_STATE", v, sizeof v);
        mode = !strcmp(v, "survey") ? 1 : !strcmp(v, "roundtrip") ? 2 : !strcmp(v, "dump") ? 3 : 0;
    }
    if (!mode) return;
    u32 count;
    Object **all = all_objects(om, &count);
    std::vector<unsigned char> blob;
    // dump: every unit's saved state at two moments, as files, to compare between two games
    static int dumps;
    if (mode == 3 && dumps < 2 && sim_ms >= (dumps ? 30000u : 6000u)) {
        char dir[MAX_PATH], path[MAX_PATH], name[80];
        sprintf_s(dir, "%s/mp_blobs_%lu_%d", g_out_dir, GetCurrentProcessId(), dumps);
        CreateDirectoryA(dir, 0);
        int n = 0;
        for (u32 i = 0; i < count; i++) {
            if (!state_is_unit(all[i]) || !state_save(all[i], blob)) continue;
            strcpy_s(name, all[i]->full);
            for (char *c = name; *c; c++) if (*c == ':') *c = '-';
            sprintf_s(path, "%s/%s.bin", dir, name);
            FILE *f = 0;
            if (!fopen_s(&f, path, "wb") && f) { fwrite(blob.data(), 1, blob.size(), f); fclose(f); n++; }
        }
        mp_log("state dump %d at %llu ms: %d units in %s", dumps, sim_ms, n, dir);
        dumps++;
    }
    if (!surveyed && sim_ms >= 20000) {
        surveyed = true;
        std::map<std::string, std::pair<int, size_t>> by_class;
        size_t total = 0;
        for (u32 i = 0; i < count; i++) {
            if (!state_is_simulation(all[i])) continue;
            try {
                if (!state_save(all[i], blob)) continue;
            } catch (...) {
                mp_log("state survey: %s failed while saving", all[i]->full);
                continue;
            }
            auto &c = by_class[all[i]->iface];
            c.first++;
            c.second += blob.size();
            total += blob.size();
        }
        mp_log("state survey at %llu ms: %zu bytes in total", sim_ms, total);
        for (auto &kv : by_class)
            mp_log("  %-18s x%-4d %9zu bytes  (%zu each)", kv.first.c_str(), kv.second.first, kv.second.second, kv.second.second / kv.second.first);
    }
    if (mode == 2 && sim_ms - last_ms >= 500) {
        static u64 rounds, bytes, underruns;
        last_ms = sim_ms;
        std::vector<Object *> units;
        for (u32 i = 0; i < count; i++) if (state_is_unit(all[i])) units.push_back(all[i]);
        for (Object *u : units) {
            if (!state_save(u, blob)) continue;
            int r = state_load(u, blob.data(), blob.size());
            bytes += blob.size();
            if (r > 0) underruns += r;
        }
        if (++rounds % 20 == 0)
            mp_log("state roundtrip: %llu rounds, %zu units, %llu KB moved, %llu underruns", rounds, units.size(), bytes / 1024, underruns);
    }
}
