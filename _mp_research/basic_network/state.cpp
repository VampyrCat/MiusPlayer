// Full object state through the engine's own savegame code.
//
// Every simulation object answers the commands "save" and "load" with a message whose +0x10 field points
// at a byte stream (the same code the game uses to save and restore a battle). Units write their plain
// state fields, their dynamic arrays, and references to other objects as stable object ids, so a blob
// saved in one game can be loaded into the same unit of another game running the same battle.
//
//   stream: +0x00 begin, +0x08 end, +0x10 capacity end, +0x18 read position, +0x1c underrun count
#include "common.h"
#include "detour.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <string>
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
        mods[0] = GetModuleHandleA("adv_physic.codelib");
        mods[1] = GetModuleHandleA("adv_game.codelib");
        mods[2] = GetModuleHandleA("basic_physic.codelib");
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
typedef void (*BaseLoad)(char *, void *);
static BaseLoad g_base_load;
static int g_remote_load;

static void base_load_hook(char *o, void *msg)
{
    if (!g_remote_load) { g_base_load(o, msg); return; }
    std::vector<u32> handles;
    {
        u32 n = *(u32 *)(o + 0x85c);
        const char *parts = *(const char **)(o + 0x850);
        if (n && n < 4096 && readable(parts, (size_t)n * 0x60))
            for (u32 i = 0; i < n; i++) {
                handles.push_back(*(const u32 *)(parts + (size_t)i * 0x60 + 0x48));
                handles.push_back(*(const u32 *)(parts + (size_t)i * 0x60 + 0x4c));
            }
    }
    g_base_load(o, msg);
    // The parts this game already had keep their handles. Parts the other game has in addition are dropped
    // from the list: there is no local handle to draw them with.
    u32 n = *(u32 *)(o + 0x85c), had = (u32)handles.size() / 2;
    char *parts = *(char **)(o + 0x850);
    if (n > had) *(u32 *)(o + 0x85c) = n = had;
    if (n && n < 4096 && readable(parts, (size_t)n * 0x60))
        for (u32 i = 0; i < n; i++) {
            *(u32 *)(parts + (size_t)i * 0x60 + 0x48) = handles[i * 2];
            *(u32 *)(parts + (size_t)i * 0x60 + 0x4c) = handles[i * 2 + 1];
        }
}

// Last line of defence. A handle is a place in the renderer's node tables (group << 10 | node). The
// renderer's node look-up (basic_render+0x13ab0) follows whatever it is given; with a handle from the
// other game that names no live node here it walks into unmapped memory and the game is gone - seen from
// several places in the unit update, always at this function. With another player's states being loaded
// the look-up is therefore wrapped: an access violation inside it ends the look-up (the caller gets the
// first transform slot) instead of ending the game.
typedef u32 (*NodeLookup)(char *, u64, int);
static NodeLookup g_node_lookup;
static u64 g_bad_lookups;

static u32 node_lookup_hook(char *render, u64 group, int node)
{
    __try {
        return g_node_lookup(render, group, node);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        g_bad_lookups++;
        return 0;
    }
}

u64 state_bad_lookups(void) { u64 n = g_bad_lookups; g_bad_lookups = 0; return n; }
bool state_remote_loading(void) { return g_remote_load > 0; }

static void install_part_guard(void)
{
    static const unsigned char sig[] = { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56 };
    static const unsigned char lookup_sig[] = { 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xec, 0x20,
                                                0x4c, 0x8b, 0x89, 0x40, 0x02, 0x00, 0x00 };
    static bool tried;
    if (tried) return;
    tried = true;
    HMODULE m = GetModuleHandleA("adv_physic.codelib"), render = GetModuleHandleA("basic_render.codelib");
    if (!m || memcmp((char *)m + 0x82090, sig, sizeof sig)) { mp_log("state: unknown game build, render handles are not guarded"); return; }
    g_base_load = (BaseLoad)install_detour((char *)m + 0x82090, 14, base_load_hook);
    if (render && !memcmp((char *)render + 0x13ab0, lookup_sig, sizeof lookup_sig))
        g_node_lookup = (NodeLookup)install_detour((char *)render + 0x13ab0, 15, node_lookup_hook);
    mp_log("state: render handles %s%s", g_base_load ? "stay local when states are loaded" : "could not be guarded",
           g_node_lookup ? ", node look-ups are guarded" : "");
}

// Is a state saved by the other game safe to load into this unit? The saved state carries render handles
// in more places than the part list guarded above, and a unit drawn through another process's handles
// crashes the renderer. Two games that built their scene in the same order hand out the same handles; once
// they differ for a unit, its state is not loaded (the unit is then followed by position only).
// The test compares the part list inside the blob with the unit's own. Layout of the base save
// (adv_physic+0x81c90): 0x296 fixed bytes, then counted lists - {u32 capacity, u32 count, records}:
// 17-byte records, 8-byte records, two object ids, u32 records, 0x40-byte records, and the parts
// (0x40 matrix, node handle, instance handle, flags).
bool state_handles_match(Object *o, const unsigned char *data, size_t size)
{
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
    const char *mine = *(const char **)((char *)o + 0x850);
    if (n != *(u32 *)((char *)o + 0x85c)) return false;
    if (n && !readable(mine, (size_t)n * 0x60)) return false;
    for (u32 i = 0; i < n; i++)
        if (memcmp(rec + (size_t)i * 0x4c + 0x40, mine + (size_t)i * 0x60 + 0x48, 8)) return false;
    return true;
}

// Loads a blob into a unit but keeps the unit's current place and motion. Position is handled by the
// smooth transform stream; a state blob is always a little older and would pull the unit back.
//
// keep_view: also keep the fields that describe the unit from this game's player's point of view. The
// engine saves them with the unit (base unit save, adv_physic+0x81c90, field tables at +0xd7e90/+0xd7fc0):
//   +0x6b0, +0x6b4, +0x6e8, +0x6ec, +0x6f0, +0x714  its place in the player's own force (-1 / 0 for the
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
int state_load_keep_motion(Object *o, const unsigned char *data, size_t size, bool keep_view)
{
    static const unsigned short view[] = { 0x6b0, 0x6b4, 0x6e8, 0x6ec, 0x6f0, 0x714, 0x72c };
    static const unsigned short view_bytes[] = { 0x745, 0x749 };
    unsigned char body[UNIT_MOTION_SIZE], kept_bytes[_countof(view_bytes)], kept_drawn;
    const unsigned short drawn = !strcmp(o->iface, "i_human") ? 0x228c : 0x1e66;
    u32 kept[_countof(view)];
    memcpy(body, (char *)o + UNIT_ROT, sizeof body);          // +0xf0 .. +0x2c0: the whole rigid body
    for (int i = 0; i < _countof(view); i++) memcpy(&kept[i], (char *)o + view[i], 4);
    for (int i = 0; i < _countof(view_bytes); i++) kept_bytes[i] = ((unsigned char *)o)[view_bytes[i]];
    kept_drawn = ((unsigned char *)o)[drawn];

    // a soldier's animation instance (+0x1d60) and its second render handle (+0x1d64) are process-local too
    bool human = !strcmp(o->iface, "i_human");
    u32 anim[2];
    memcpy(anim, (char *)o + 0x1d60, 8);

    // A vehicle keeps a list of points fixed to nodes of its model (+0x1298: 0x20-byte records, the node's
    // render handle at +0x10; count at +0x12a4, room at +0x12a8). It is made when the model is, and saved
    // raw with the vehicle - handles and all. This game's handles are the only ones that mean anything here.
    bool techn = !strcmp(o->iface, "i_techn");
    std::vector<unsigned char> points;
    if (techn) {
        u32 n = *(u32 *)((char *)o + 0x12a4);
        const unsigned char *p = *(const unsigned char **)((char *)o + 0x1298);
        if (n && n < 4096 && readable(p, (size_t)n * 0x20)) points.assign(p, p + (size_t)n * 0x20);
    }
    // Two more lists of a vehicle start every record with a pointer to a description (+0x15e8: records of
    // 0x120 bytes, count at +0x15f4; +0x1600: records of 0x1d0 bytes, count at +0x160c). They are written
    // as description numbers, and a number of the other game does not always name the same description
    // here; the vehicle update then reads a "name" out of something that is not one (the crash at
    // adv_game+0xd9917 that kept state replication out of operation battles). What a vehicle is fitted
    // with does not change during a battle, so this game's own pointers are kept.
    static const struct { unsigned short list, count, size; } k_fitted[] = { { 0x15e8, 0x15f4, 0x120 }, { 0x1600, 0x160c, 0x1d0 } };
    std::vector<unsigned char> fitted[2];
    if (techn)
        for (int k = 0; k < 2; k++) {
            u32 n = *(u32 *)((char *)o + k_fitted[k].count);
            const unsigned char *p = *(const unsigned char **)((char *)o + k_fitted[k].list);
            if (n && n < 1024 && readable(p, (size_t)n * k_fitted[k].size)) fitted[k].assign(p, p + (size_t)n * k_fitted[k].size);
        }

    install_part_guard();
    g_remote_load++;
    int r = state_load(o, data, size);
    g_remote_load--;
    if (human) memcpy((char *)o + 0x1d60, anim, 8);
    if (techn)
        for (int k = 0; k < 2; k++) {
            u32 n = *(u32 *)((char *)o + k_fitted[k].count), room = *(u32 *)((char *)o + k_fitted[k].count + 4);
            u32 had = (u32)(fitted[k].size() / k_fitted[k].size);
            unsigned char *p = *(unsigned char **)((char *)o + k_fitted[k].list);
            if (!had) {
                if (n) *(u32 *)((char *)o + k_fitted[k].count) = 0;      // this game's vehicle has none: nor has it now
            } else if (n == had && readable(p, fitted[k].size())) {
                // same fittings: their state is the owner's, the descriptions they point at are this game's
                for (u32 i = 0; i < n; i++) memcpy(p + (size_t)i * k_fitted[k].size, fitted[k].data() + (size_t)i * k_fitted[k].size, 8);
            } else if (room >= had && readable(p, fitted[k].size())) {
                memcpy(p, fitted[k].data(), fitted[k].size());           // another set over there: this game's stays whole
                *(u32 *)((char *)o + k_fitted[k].count) = had;
            }
        }
    if (techn && !points.empty()) {
        u32 n = *(u32 *)((char *)o + 0x12a4), room = *(u32 *)((char *)o + 0x12a8), had = (u32)(points.size() / 0x20);
        unsigned char *p = *(unsigned char **)((char *)o + 0x1298);
        if (n == had && readable(p, points.size())) {
            for (u32 i = 0; i < n; i++) memcpy(p + (size_t)i * 0x20 + 0x10, points.data() + (size_t)i * 0x20 + 0x10, 4);
        } else if (room >= had && readable(p, points.size())) {
            // the other game has another model for this vehicle by now (a wreck, say): this game's list stays
            memcpy(p, points.data(), points.size());
            *(u32 *)((char *)o + 0x12a4) = had;
        }
    }

    memcpy((char *)o + UNIT_ROT, body, sizeof body);
    if (keep_view) {
        for (int i = 0; i < _countof(view); i++) memcpy((char *)o + view[i], &kept[i], 4);
        for (int i = 0; i < _countof(view_bytes); i++) ((unsigned char *)o)[view_bytes[i]] = kept_bytes[i];
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
