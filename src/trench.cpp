// Trenches. A force that is set to defend starts the battle dug in, and while its player places his
// squads during deployment each one is dug in again where he puts it; after the battle has started nobody
// digs. All of it goes through one function of the trench manager (i_trencher, adv_game): virtual slot 3,
// adv_game+0xdbc20: dig(trencher, squad id, position or 0). A trench is a ready-made piece of ground, 32 m
// square, stamped onto one cell of the map (the generator is adv_game+0xdc9a0: map tile, cell in the tile,
// kind of trench, one of eight directions, variant). Which cell and which direction is in the squad's
// record: +0x33c tile, +0x338 cell, +0x334 direction, +0x328 kind - kept up to date by the game of the
// player who places the squad, and by nothing in the other game: there the squad's record still says where
// that game itself first put the squad. So each game dug for every squad from what it knew itself:
// trenches in other places, facing other ways, and since a trench changes the ground, vehicles crossing
// one were shown displaced on the other PC.
//
// Digging is therefore the owner's, like firing (shots.cpp):
//   - a dig for a squad this game's player commands happens as usual, and what went into it is sent to
//     the other game: the squad's dig-in settings, the kind of trench, the place if one was given, where
//     the squad's leading unit stands and how it is turned, and the trench manager's random state;
//   - a dig this game would make for one of the other player's squads is dropped;
//   - a dig received from the other game is carried out here from the owner's values, in the order the
//     owner made them (the last one for a squad is where it ends up).
//     What is stamped onto the map is not worked out a second time: the owner's dig notes the call it made
//     to the generator (or that it made none - the cell was taken, say), and the dig carried out here
//     makes exactly that call. A dig is a result that is passed on, not a cause that is replayed.
// The records travel on the same reliable, ordered channel as the shots (ShotRec.kind 2).
// MP_SYNC_TRENCH=0 switches this off; MP_LOG_TRENCH=1 logs every dig.
// beta26 (public_beta-26494568): dig +0x154f70, generator +0x155e50, crater +0x157d30, add place +0x15cd80; the
// squad record is 9920 bytes and its trench fields are 0x68 further on (SQ_* below).
#include "common.h"
#include "build.h"
#include "protocol.h"
#include "detour.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// The squad record's fields this file uses (i_divisions slot 11; beta26 offsets read in the dig +0x154f70, the
// trench maker +0x155c60 it calls and the record's save +0xbc060): kind of trench, direction, cell, tile, dig-in
// settings, cover, the two switches the dig asks for, "this game's player commands the squad"; SQ_SIZE what is read.
#define SQ_KIND  BF(0x328, 0x390)
#define SQ_DIR   BF(0x334, 0x39c)
#define SQ_CELL  BF(0x338, 0x3a0)
#define SQ_TILE  BF(0x33c, 0x3a4)
#define SQ_FLAGS BF(0x340, 0x3a8)
#define SQ_COVER BF(0x3c0, 0x428)
#define SQ_B3E3  BF(0x3e3, 0x44b)
#define SQ_B3E4  BF(0x3e4, 0x44c)
#define SQ_USER  BF(0x3ef, 0x4dc)
#define SQ_SIZE  BF(0x400, 0x4e0)

typedef u64 (*Dig)(char *trencher, int squad, float *pos);
typedef u64 (*Gen)(char *trencher, void *ctx, u32 tile, u64 cell, u32 type, u32 dir, int variant, u32 flags, char *squad, char own);
static Dig g_dig;
static Gen g_gen;
struct GenCall { u32 tile, cell, type, dir; int variant; u32 flags; unsigned char with_squad, own; };
static GenCall g_gen_seen;                  // the generator call made by the dig in progress ...
static int g_gen_count;                     // ... and how many it made
static bool g_in_dig;
static const GenCall *g_gen_use;            // a dig of the other player being carried out: his call, or none
static bool g_gen_used;
static u64 g_stat_stamped, g_stat_stamp_added, g_stat_stamp_held, g_stat_stamp_other;
static char *g_trencher;
static bool g_injecting, g_enabled = true, g_trace;
static DWORD g_main_thread;
static u64 g_stat_own, g_stat_dropped, g_stat_applied, g_stat_missing;
static std::vector<ShotRec> g_wait;         // received, carried out before the next simulation step
static std::vector<ShotRec> g_mine;         // squads this game's player has placed, for a player who connects later
static std::vector<ShotRec> g_all;          // tests: every dig of this player's squads
// Trenches stamped onto the map outside any dig. An operation battle has them: units that held their
// ground since the last battle bring their trenches along (the game asks the generator for them through
// the manager's virtual slot 6 while the battle is built) - in the host's game only, because only the host's
// battle belongs to the operation. Each game keeps the list of those it has stamped and gives it to the
// other game, which stamps the ones it has not got (ShotRec.kind 6).
static std::vector<ShotRec> g_stamp_log;
// (The same cell is stamped more than once, with other flags: 0x23 while the battle is built - the trench is
// in the ground already, it is only marked - and 0x1d when deployment ends, which is what gives the trench
// its places for soldiers. So a stamp is known by all its values, not by its cell.)
static std::unordered_set<u64> g_stamp_have;        // every stamp made here, by all its values
static u64 stamp_key(u32 tile, u32 cell, u32 type, u32 dir, u32 variant, u32 flags)
{
    u32 v[6] = { tile, cell, type, dir, variant, flags };
    u64 h = 1469598103934665603ull;
    for (int i = 0; i < 24; i++) h = (h ^ ((const unsigned char *)v)[i]) * 1099511628211ull;
    return h;
}
static std::vector<ShotRec> g_stamp_wait;
static bool g_stamp_injecting, g_lists_due = true;
static u64 g_stat_stamp_sent, g_stat_stamp_made, g_stat_stamp_had, g_stat_stamp_twice;
// Stamps made here on the other game's word that this game has not made itself (yet), with what the
// generator answered. With both games building an operation's battle from the same files, both stamp the
// old trenches themselves - the second time, with their places for soldiers, in the moments deployment ends
// in both games. Each tells the other; the game that got the other's list a few milliseconds before its own
// turn stamped all of them twice (a Peleshi battle: 18 trenches, 191 places held twice in the host's game,
// in other runs 120 - never in the joined game, whose own turn comes first). A stamp this game is about to
// make that it has made on the other game's word already is therefore left out.
static std::unordered_map<u64, u64> g_stamp_listed;

#pragma pack(push, 1)
struct TrenchRec {                          // travels in ShotRec.launch
    u32 squad, has_pos;
    float pos[4];
    u32 flags, type, seed;                  // squad record SQ_FLAGS (+0x340), SQ_KIND (+0x328); trench manager +0x2d0
    unsigned char b3e3, b3e4, b3ef, has_leader;     // SQ_B3E3, SQ_B3E4, SQ_USER
    u64 leader;                             // the squad's leading unit (squad record +0x40) ...
    float lpos[3], lrot[9];                 // ... and where it stands and how it is turned
    u32 dir, cell, tile, cover;             // squad record SQ_DIR, SQ_CELL, SQ_TILE: where the trench goes; SQ_COVER
    unsigned char stamped, g_with_squad, g_own, pad;    // what the owner's dig had stamped onto the map:
    u32 g_tile, g_cell, g_type, g_dir, g_flags;         // the generator's arguments
    int g_variant;
};
#pragma pack(pop)
static_assert(sizeof(TrenchRec) <= sizeof(((ShotRec *)0)->launch), "a dig has to fit into a shot record");

// ---------------------------------------------------------------- craters
// The same manager makes the craters: virtual slot 5, adv_game+0xde660(trencher, place, kind < 8, a vector,
// flags, size) -> +0xde720, which presses the hole into the ground and has its mark drawn. Which of several
// shapes, and how it is turned, comes from the manager's random numbers (+0x2d0); whether a mark is drawn
// depends on the trenches near by. Both games work every shell out for themselves, so each made its own
// craters: not quite in the same places, with other shapes - other ground under the units.
// The ground is the host's: a crater the host's game makes is sent with everything that went into it
// (place, kind, vector, flags, size, random state) and made here from those values; the craters this game
// would make by itself are dropped while a host is connected. ShotRec.kind 5, on the shots' channel.
// beta26 (slot 5 +0x157c60 -> +0x157d30) takes one more argument, the variant: 0xffffffff draws the shape and its
// turn from the random numbers as before, otherwise the shape is its low 16 bits and the turn bits 16-17 (the
// shell's explosion, basic_physic+0x3db30, passes 0xffffffff). It travels with the crater; 2024: 0xffffffff.
#pragma pack(push, 1)
struct CraterRec { float pos[4]; int kind; u32 flags; float size; u32 seed; float vec[4]; u32 variant; };
#pragma pack(pop)
typedef void (*Crater)(char *trencher, void *ctx, float *pos, int kind, float *vec, u32 flags, float size, u32 variant);
static Crater g_crater;
static bool g_crater_injecting, g_crater_sync = true;
static std::vector<ShotRec> g_crater_wait;
static std::vector<ShotRec> g_crater_log;           // host: every crater since its battle began to simulate;
                                                    // joined game: the ones it made before it was connected
static u64 g_stat_crater_own, g_stat_crater_dropped, g_stat_crater_applied, g_stat_crater_other_thread, g_stat_crater_had, g_stat_crater_logged;

// Which player this battle's game is, also before the session has started (while the battle is built).
static int battle_role(void)
{
    int role = session_role();
    if (role != ROLE_NONE) return role;
    char v[16] = "";
    GetEnvironmentVariableA("MP_ROLE", v, sizeof v);
    return !strcmp(v, "host") ? ROLE_HOST : v[0] ? ROLE_JOIN : ROLE_NONE;
}

static void crater_hook(char *trencher, void *ctx, float *pos, int kind, float *vec, u32 flags, float size, u32 variant)
{
    g_trencher = trencher;
    if (g_bi != BUILD_BETA26) variant = 0xffffffffu;        // (the 2024 function has no 8th argument)
    int role = battle_role();
    if (g_crater_injecting || !g_crater_sync || role == ROLE_NONE || GetCurrentThreadId() != g_main_thread) {
        if (role != ROLE_NONE && !g_crater_injecting && g_crater_sync) g_stat_crater_other_thread++;        // (not seen)
        g_crater(trencher, ctx, pos, kind, vec, flags, size, variant);
        return;
    }
    bool host = role == ROLE_HOST;
    bool connected = session_simulating() && session_connected() && session_versus_or_coop();
    if (!host && connected) { g_stat_crater_dropped++; return; }        // the host's game makes it and says so
    // Made here: by the host at any time, by a joined game until it is connected - while it builds its battle
    // and in its first moments. All of them are remembered. The host's list goes to a game that connects
    // (an operation brings the craters of its earlier battles: a second PC has no record of those); the
    // joined game's list tells it which of the host's it has made itself already.
    ShotRec s = {};
    CraterRec c = {};
    if (readable(pos, 16)) memcpy(c.pos, pos, 16);
    if (readable(vec, 16)) memcpy(c.vec, vec, 16);
    c.kind = kind; c.flags = flags; c.size = size; c.variant = variant;
    c.seed = *(u32 *)(trencher + 0x2d0);
    g_crater(trencher, ctx, pos, kind, vec, flags, size, variant);
    s.kind = 5;
    memcpy(s.launch, &c, sizeof c);
    if (g_crater_log.size() < 60000) g_crater_log.push_back(s);
    if (!host) return;
    g_stat_crater_own++;
    if (g_trace && session_simulating() && g_stat_crater_logged++ < 300)
        mp_log("crater: kind %d at %.1f %.1f %.1f, flags %x, size %.2f, vector %.2f %.2f %.2f %.2f%s", kind, c.pos[0], c.pos[1], c.pos[2],
               flags, size, c.vec[0], c.vec[1], c.vec[2], c.vec[3], connected ? " - sent" : "");
    if (connected) shots_queue(s);
}

void crater_received(const ShotRec &s)
{
    if (g_crater_wait.size() < 8192) g_crater_wait.push_back(s);
}

static void craters_apply(ObjMan *om)
{
    if (g_crater_wait.empty()) return;
    if (!g_trencher) g_trencher = (char *)engine_find(om, "i_trencher", "trencher");
    if (!g_crater || !g_trencher) { g_crater_wait.clear(); return; }
    void **vt = *(void ***)g_trencher;
    for (const ShotRec &s : g_crater_wait) {
        CraterRec c;
        memcpy(&c, s.launch, sizeof c);
        float pos[4], vec[4];
        memcpy(pos, c.pos, 16);
        memcpy(vec, c.vec, 16);
        // one this game made itself before it was connected (the same crater, from the same cause)?
        if (session_role() != ROLE_HOST) {
            // (by how many: the game makes the craters an operation brings twice over, at the same places)
            static std::unordered_map<u64, int> mine, theirs;
            static size_t built;
            auto key = [](const CraterRec &r) {
                return (u64)(u32)(int)floorf(r.pos[0] * 20.0f + 0.5f) << 40 ^ (u64)(u32)(int)floorf(r.pos[2] * 20.0f + 0.5f) << 12 ^ (u64)(u32)r.kind;
            };
            for (; built < g_crater_log.size(); built++) {
                CraterRec m;
                memcpy(&m, g_crater_log[built].launch, sizeof m);
                mine[key(m)]++;
            }
            u64 k = key(c);
            auto own = mine.find(k);
            if (own != mine.end() && ++theirs[k] <= own->second) { g_stat_crater_had++; continue; }
        }
        *(u32 *)(g_trencher + 0x2d0) = c.seed;
        g_crater_injecting = true;
        // through the manager's own entry (slot 5): it loads what the crater needs if this game has not yet
        // (beta26: with the owner's variant)
        ((void (*)(char *, float *, int, float *, u32, float, u32))vt[5])(g_trencher, pos, c.kind, vec, c.flags, c.size, c.variant);
        g_crater_injecting = false;
        g_stat_crater_applied++;
    }
    g_crater_wait.clear();
}

// ---------------------------------------------------------------- diagnostics: who fills the store
// MP_TRACE_PLACES=1: every place added to the store (adv_game+0xe0990) is counted by the code that asked
// for it, three callers deep; the table is logged when the battle starts to simulate.
typedef u32 (*AddPlace)(char *trencher, float *pos);
static AddPlace g_add_place;
struct PlaceFrom { void *f[3]; u64 n; };
static std::vector<PlaceFrom> g_place_from;
static u32 add_place_hook(char *trencher, float *pos)
{
    void *frames[5] = {};
    RtlCaptureStackBackTrace(1, 5, frames, 0);
    bool found = false;
    for (PlaceFrom &p : g_place_from)
        if (p.f[0] == frames[0] && p.f[1] == frames[1] && p.f[2] == frames[2]) { p.n++; found = true; break; }
    if (!found && g_place_from.size() < 64) { PlaceFrom p = { { frames[0], frames[1], frames[2] }, 1 }; g_place_from.push_back(p); }
    return g_add_place(trencher, pos);
}

static void report_places(void)
{
    static bool done;
    if (done || g_place_from.empty()) return;
    done = true;
    for (const PlaceFrom &p : g_place_from) {
        char text[3][96];
        for (int i = 0; i < 3; i++) {
            HMODULE m = 0;
            char path[MAX_PATH] = "?";
            if (p.f[i] && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)p.f[i], &m) && m)
                GetModuleFileNameA(m, path, sizeof path);
            const char *base = strrchr(path, '\\');
            sprintf_s(text[i], "%s+0x%llx", base ? base + 1 : path, m ? (u64)((char *)p.f[i] - (char *)m) : 0);
        }
        mp_log("places: %llu added from %s <- %s <- %s", p.n, text[0], text[1], text[2]);
    }
}

// The squad's record in i_divisions (trench manager +0x1b8, virtual slot 11: record by squad or unit id).
static char *squad_record(char *trencher, u32 squad)
{
    void **div = *(void ***)(trencher + 0x1b8);
    if (!readable(div, 8) || !readable(*div, 0x60)) return 0;
    char *rec = ((char *(*)(void *, u32))(*(void ***)div)[11])(div, squad);
    return readable(rec, SQ_SIZE) ? rec : 0;
}

// The generator: adv_game+0xdc9a0(trencher, template pack, map tile, cell, kind, direction, variant, flags,
// squad record or 0, flag). Called from inside a dig (and by the game for the trenches a map comes with -
// those calls pass through).
static u64 gen_hook(char *trencher, void *ctx, u32 tile, u64 cell, u32 type, u32 dir, int variant, u32 flags, char *squad, char own)
{
    if (g_injecting) {
        if (!g_gen_use || g_gen_used) { g_stat_stamp_held++; return 0; }     // his game stamped nothing (more)
        g_gen_used = true;
        const GenCall &c = *g_gen_use;
        g_stat_stamped++;
        g_stamp_have.insert(stamp_key(c.tile, c.cell, c.type, c.dir, (u32)c.variant, c.flags));
        return g_gen(trencher, ctx, c.tile, c.cell, c.type, c.dir, c.variant, c.flags, c.with_squad ? squad : 0, (char)c.own);
    }
    u64 key = stamp_key(tile, (u32)cell, type, dir, (u32)variant, flags);
    if (g_stamp_injecting) {
        g_stamp_have.insert(key);
        u64 answer = g_gen(trencher, ctx, tile, cell, type, dir, variant, flags, squad, own);
        g_stamp_listed[key] = answer;
        return answer;
    }
    if (!g_in_dig && !squad) {
        auto listed = g_stamp_listed.find(key);
        if (listed != g_stamp_listed.end()) {
            // in the ground already, on the other game's word: not a second time
            u64 answer = listed->second;
            g_stamp_listed.erase(listed);
            g_stat_stamp_twice++;
            ShotRec s = {};
            u32 rec[6] = { tile, (u32)cell, type, dir, (u32)variant, flags };
            s.kind = 6;
            memcpy(s.launch, rec, sizeof rec);
            bool known = false;
            for (const ShotRec &o : g_stamp_log) known |= !memcmp(o.launch, rec, sizeof rec);
            if (!known && g_stamp_log.size() < 8192) g_stamp_log.push_back(s);       // (for a game that connects later)
            if (g_trace) mp_log("trench: this game's own stamp left out, made on the other game's word already: tile %u cell %u kind %u direction %u variant %d flags %x",
                                tile, (u32)cell, type, dir, variant, flags);
            return answer;
        }
    }
    g_stamp_have.insert(key);
    if (g_in_dig) {
        if (!g_gen_count++) {
            GenCall c = { tile, (u32)cell, type, dir, variant, flags, (unsigned char)(squad != 0), (unsigned char)own };
            g_gen_seen = c;
        }
    } else {
        // (asked for through the manager's slot 6: the same call can be made again by the other game)
        ShotRec s = {};
        u32 rec[6] = { tile, (u32)cell, type, dir, (u32)variant, flags };
        s.kind = 6;
        memcpy(s.launch, rec, sizeof rec);
        bool known = false;
        for (const ShotRec &o : g_stamp_log) known |= !memcmp(o.launch, rec, sizeof rec);
        if (!known && !squad && g_stamp_log.size() < 8192) {
            g_stamp_log.push_back(s);
            if (session_connected() && session_simulating() && g_enabled) { shots_queue(s); g_stat_stamp_sent++; }
        }
        g_stat_stamp_other++;
        if (g_trace && g_stat_stamp_other <= 400) {
            void *from[2] = {};
            RtlCaptureStackBackTrace(1, 2, from, 0);
            char *m = build_module("adv_game.codelib");
            mp_log("trench: stamped outside a dig: tile %u cell %u kind %u direction %u variant %d flags %x squad %s (%s), asked for by adv_game+0x%llx <- +0x%llx",
                   tile, (u32)cell, type, dir, variant, flags, squad ? "yes" : "no", session_simulating() ? "battle running" : "while the battle is built",
                   (u64)((char *)from[0] - (char *)m), (u64)((char *)from[1] - (char *)m));
        }
    }
    return g_gen(trencher, ctx, tile, cell, type, dir, variant, flags, squad, own);
}

static u64 dig_hook(char *trencher, int squad, float *pos)
{
    g_trencher = trencher;
    // Digging is the owner's once this game simulates. While it still builds its battle it digs for every
    // squad itself, as the other game did when it built its own: a game that joins a large battle is
    // connected before it has finished building, and the host's squads it then left undug stayed undug -
    // the host dug them before anybody was there to be told (a Peleshi battle: 190 to 310 places fewer).
    bool connected = session_connected() && g_enabled && session_simulating();
    if (g_injecting || session_role() == ROLE_NONE || GetCurrentThreadId() != g_main_thread) return g_dig(trencher, squad, pos);
    char *sq = squad_record(trencher, (u32)squad);
    if (connected && !session_owns_squad((u32)squad)) {
        g_stat_dropped++;
        if (g_trace) mp_log("trench: dig for squad %08x of the other player dropped", (u32)squad);
        return 0;
    }
    ShotRec s = {};
    TrenchRec r = {};
    r.squad = (u32)squad;
    r.has_pos = pos != 0;
    if (pos) memcpy(r.pos, pos, 16);
    r.seed = *(u32 *)(trencher + 0x2d0);
    if (sq) {
        r.flags = *(u32 *)(sq + SQ_FLAGS);
        r.type = *(u32 *)(sq + SQ_KIND);
        r.b3e3 = sq[SQ_B3E3]; r.b3e4 = sq[SQ_B3E4]; r.b3ef = sq[SQ_USER];
        r.dir = *(u32 *)(sq + SQ_DIR); r.cell = *(u32 *)(sq + SQ_CELL); r.tile = *(u32 *)(sq + SQ_TILE); r.cover = *(u32 *)(sq + SQ_COVER);
        Object *leader = *(Object **)(sq + 0x40);
        if (readable(leader, 0x2c0) && session_unit_key(leader, &r.leader)) {
            r.has_leader = 1;
            const float *p = (const float *)((char *)leader + UNIT_POS), *rot = (const float *)((char *)leader + UNIT_ROT);
            memcpy(r.lpos, p, 12);
            for (int row = 0; row < 3; row++) memcpy(r.lrot + row * 3, rot + row * 4, 12);
        }
    }
    g_in_dig = true;
    g_gen_count = 0;
    u64 result = g_dig(trencher, squad, pos);
    g_in_dig = false;
    if (g_gen_count) {
        const GenCall &c = g_gen_seen;
        r.stamped = 1; r.g_with_squad = c.with_squad; r.g_own = c.own;
        r.g_tile = c.tile; r.g_cell = c.cell; r.g_type = c.type; r.g_dir = c.dir; r.g_flags = c.flags; r.g_variant = c.variant;
    }
    g_stat_own++;
    if (g_trace) {
        char what[120] = "stamped nothing";
        if (r.stamped) sprintf_s(what, "stamped tile %u cell %u kind %u direction %u variant %d flags %x (%d calls)", r.g_tile, r.g_cell, r.g_type, r.g_dir, r.g_variant, r.g_flags, g_gen_count);
        mp_log("trench: squad %08x dug in (kind %u, settings %x %u %u %u, tile %u cell %u direction %u, %s): %s%s", r.squad, r.type, r.flags,
               r.b3e3, r.b3e4, r.b3ef, r.tile, r.cell, r.dir, pos ? "at a given place" : "where it stands", what, connected ? ", sent" : "");
    }
    s.kind = 2;
    memcpy(s.launch, &r, sizeof r);
    // What is kept for a player who connects later: the squads this player has placed himself. The digging
    // done when the battle is built comes out the same in both games by itself (same battle, nobody has
    // moved anything yet) - sent again it would be dug twice there.
    if (r.has_pos && g_mine.size() < 4096) g_mine.push_back(s);
    if (g_all.size() < 4096) g_all.push_back(s);
    if (connected) shots_queue(s);
    return result;
}

// A player has connected (again): he gets this game's digs of its own player's squads.
void stamp_received(const ShotRec &s)
{
    if (g_stamp_wait.size() < 8192) g_stamp_wait.push_back(s);
}

// Before a simulation step: this game's lists go to a game that has just connected, and the stamps the
// other game has sent are made here unless this game has that trench already.
static void stamps_step(ObjMan *om)
{
    if (g_lists_due && g_enabled && g_gen && session_connected() && session_simulating()) {
        g_lists_due = false;
        for (ShotRec s : g_stamp_log) { shots_queue(s); g_stat_stamp_sent++; }
        if (session_role() == ROLE_HOST && g_crater_sync)
            for (ShotRec s : g_crater_log) shots_queue(s);
        if (!g_stamp_log.empty() || !g_crater_log.empty())
            mp_log("trench: the other game is told of %zu trenches stamped here outside any dig%s", g_stamp_log.size(),
                   session_role() == ROLE_HOST ? " and of the craters made here so far" : "");
    }
    if (g_stamp_wait.empty()) return;
    if (!g_trencher) g_trencher = (char *)engine_find(om, "i_trencher", "trencher");
    if (!g_gen || !g_trencher || !g_enabled) { g_stamp_wait.clear(); return; }
    void **vt = *(void ***)g_trencher;
    for (const ShotRec &s : g_stamp_wait) {
        u32 r[6];
        memcpy(r, s.launch, sizeof r);
        if (g_stamp_have.count(stamp_key(r[0], r[1], r[2], r[3], r[4], r[5]))) { g_stat_stamp_had++; continue; }
        g_stamp_injecting = true;
        ((void (*)(char *, u32, u32, u32, u32, int, u32))vt[6])(g_trencher, r[0], r[1], r[2], r[3], (int)r[4], r[5]);
        g_stamp_injecting = false;
        g_stat_stamp_made++;
        if (g_trace) mp_log("trench: stamped from the other game's list: tile %u cell %u kind %u direction %u variant %d flags %x", r[0], r[1], r[2], r[3], (int)r[4], r[5]);
    }
    g_stamp_wait.clear();
}

// Host: a new extra player (shots channel ch) is told once what this game's ground has had done to it so far: the
// digs, the trenches stamped outside a dig and the craters.
void trench_send_all_to(int ch)
{
    if (!g_enabled) return;
    int was = shots_target(ch);
    if (g_dig)
        for (ShotRec s : g_mine) {
            TrenchRec r;
            memcpy(&r, s.launch, sizeof r);
            if (session_owns_squad(r.squad)) shots_queue(s);
        }
    for (ShotRec s : g_stamp_log) shots_queue(s);
    if (session_role() == ROLE_HOST && g_crater_sync)
        for (ShotRec s : g_crater_log) shots_queue(s);
    shots_target(was);
}

void trench_resend(void)
{
    g_lists_due = true;
    if (!g_enabled || !g_dig) return;
    for (ShotRec s : g_mine) {
        TrenchRec r;
        memcpy(&r, s.launch, sizeof r);
        if (session_owns_squad(r.squad)) shots_queue(s);
    }
}

static void apply(const TrenchRec &r)
{
    char *sq = squad_record(g_trencher, r.squad);
    if (!sq) { g_stat_missing++; return; }
    *(u32 *)(sq + SQ_FLAGS) = r.flags;
    *(u32 *)(sq + SQ_KIND) = r.type;
    // SQ_USER (+0x3ef): "this game's player commands the squad" (the division loader sets it from the squad's 'USER' tag).
    // It is the sender's for the dig only: kept, the joining player's squads became unselectable in his game
    // as soon as the host's game (mpmod2: the one that digs for every squad) sent their digs.
    char own = sq[SQ_USER];
    sq[SQ_B3E3] = r.b3e3; sq[SQ_B3E4] = r.b3e4; sq[SQ_USER] = r.b3ef;
    *(u32 *)(sq + SQ_DIR) = r.dir; *(u32 *)(sq + SQ_CELL) = r.cell; *(u32 *)(sq + SQ_TILE) = r.tile; *(u32 *)(sq + SQ_COVER) = r.cover;
    if (r.has_leader) {
        if (Object *u = session_unit(r.leader)) {
            float *p = (float *)((char *)u + UNIT_POS), *rot = (float *)((char *)u + UNIT_ROT), *inv = (float *)((char *)u + UNIT_INV);
            memcpy(p, r.lpos, 12);
            for (int row = 0; row < 3; row++)
                for (int col = 0; col < 3; col++) {
                    rot[row * 4 + col] = r.lrot[row * 3 + col];
                    inv[col * 4 + row] = r.lrot[row * 3 + col];
                }
        }
    }
    *(u32 *)(g_trencher + 0x2d0) = r.seed;
    float pos[4];
    memcpy(pos, r.pos, 16);
    GenCall his = { r.g_tile, r.g_cell, r.g_type, r.g_dir, r.g_variant, r.g_flags, r.g_with_squad, r.g_own };
    g_gen_use = r.stamped ? &his : 0;
    g_gen_used = false;
    g_injecting = true;
    g_dig(g_trencher, (int)r.squad, r.has_pos ? pos : 0);
    if (g_gen && r.stamped && !g_gen_used) {
        // this game's dig did not get as far as stamping (its own tests said no): the owner's did, so it is
        // done here all the same - with the template pack the dig has just made sure is loaded (the pack's
        // one-slot vtable the dig puts before it: adv_game+0x12d048, beta26 +0x1b8ac8)
        char *ag = build_module("adv_game.codelib");
        void *ctx[2] = { ag ? ag + BV(0x12d048, 0x12d048, 0x1b8ac8) : 0, *(void **)(g_trencher + 0x240) };
        if (ctx[0] && ctx[1]) {
            g_gen(g_trencher, ctx, his.tile, his.cell, his.type, his.dir, his.variant, his.flags, his.with_squad ? sq : 0, (char)his.own);
            g_stat_stamp_added++;
        }
    }
    g_injecting = false;
    g_gen_use = 0;
    sq[SQ_USER] = own;
    g_stat_applied++;
    if (g_trace) mp_log("trench: squad %08x of the other player dug in here from his values (kind %u, settings %x, tile %u cell %u direction %u): his game %s, this game's dig %s",
                        r.squad, r.type, r.flags, r.tile, r.cell, r.dir, r.stamped ? "stamped a trench" : "stamped nothing",
                        g_gen_used ? "made his stamp" : r.stamped ? "did not get to stamping" : "stamped nothing either");
}

// A dig of the other game has arrived (called in order, before a simulation step).
void trench_received(const ShotRec &s)
{
    if (g_wait.size() < 4096) g_wait.push_back(s);
}

// Tests: "dig <n> <metres>" - this game's player puts n of his squads somewhere else during deployment.
// The game's deployment screen then digs the squad in at the new place through the same function; that
// call is made here (the units themselves stay where they are - it is the trench that is compared).
static int g_test_squads;
static float g_test_shift;
void trench_test_move(int squads, float metres) { g_test_squads = squads; g_test_shift = metres; }

static void test_move(ObjMan *om)
{
    int n = g_test_squads, moved = 0, tried = 0;
    g_test_squads = 0;
    if (!g_trencher) g_trencher = (char *)engine_find(om, "i_trencher", "trencher");
    if (!g_dig || !g_trencher) { mp_log("test: no trench manager to dig with"); return; }
    for (u32 squad : session_own_squads()) {
        if (moved >= n) break;
        char *sq = squad_record(g_trencher, squad);
        // (only a squad that digs in at all: dig-in setting on, and the two switches the dig asks for)
        if (!sq || !(*(u32 *)(sq + SQ_FLAGS) & 1) || !sq[SQ_B3E3] || !sq[SQ_B3E4]) continue;
        Object *leader = *(Object **)(sq + 0x40);
        if (!readable(leader, 0x2c0)) continue;
        const float *p = (const float *)((char *)leader + UNIT_POS);
        float pos[4] = { p[0] + (tried % 2 ? -g_test_shift : g_test_shift), p[1], p[2] + g_test_shift * 0.6f, 0 };
        tried++;
        u64 before = g_stat_own;
        dig_hook(g_trencher, (int)squad, pos);
        if (g_stat_own != before) moved++;
    }
    mp_log("test: %d squads of this player dug in again %.0f m away", moved, g_test_shift);
}

// Before a simulation step: the digs received from the other game are carried out.
void trench_sim_step(ObjMan *om)
{
    report_places();
    stamps_step(om);
    craters_apply(om);
    if (g_test_squads) test_move(om);
    if (g_wait.empty()) return;
    if (!g_dig || !g_enabled) { g_wait.clear(); return; }
    if (!g_trencher) g_trencher = (char *)engine_find(om, "i_trencher", "trencher");
    if (!g_trencher) return;
    for (const ShotRec &s : g_wait) {
        TrenchRec r;
        memcpy(&r, s.launch, sizeof r);
        apply(r);
    }
    g_wait.clear();
}

// Called at the first loading step of a battle: the hooks are in before the battle is built, so what is put
// into the ground while it is built is seen too. Only in a battle started as a multiplayer one.
void trench_early(void)
{
    char v[16];
    if (GetEnvironmentVariableA("MP_ROLE", v, sizeof v) && v[0]) trench_install();
}

void trench_install(void)
{
    static const unsigned char sig[] = { 0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57 };
    static bool done;
    if (done) return;
    const char *ag = "adv_game.codelib";
    if (!build_module(ag)) return;
    done = true;
    g_main_thread = GetCurrentThreadId();
    char v[16];
    if (GetEnvironmentVariableA("MP_SYNC_TRENCH", v, sizeof v)) g_enabled = atoi(v) != 0;
    g_trace = GetEnvironmentVariableA("MP_LOG_TRENCH", v, sizeof v) && atoi(v);
    if (!g_enabled && !g_trace) { mp_log("trench: each game digs for itself (MP_SYNC_TRENCH=0)"); return; }
    // (signatures: whole instructions, none position dependent; the detour copies the first `len` bytes of them)
    if (GetEnvironmentVariableA("MP_TRACE_PLACES", v, sizeof v) && atoi(v)) {
        static const unsigned char add_sig[] = { 0x4c, 0x8b, 0xdc, 0x49, 0x89, 0x5b, 0x10, 0x49, 0x89, 0x73, 0x18, 0x57, 0x48, 0x81, 0xec, 0xa0, 0x00, 0x00, 0x00 };
        // beta26 +0x15cd80: mov r11,rsp; mov [r11+20h],rbx; push rbp/rdi/r14; sub rsp,0a0h | movups xmm2,[rcx+180h]
        static const unsigned char add_sig26[] = { 0x4c, 0x8b, 0xdc, 0x49, 0x89, 0x5b, 0x20, 0x55, 0x57, 0x41, 0x56, 0x48, 0x81, 0xec, 0xa0, 0x00, 0x00, 0x00,
                                                   0x0f, 0x10, 0x91, 0x80, 0x01, 0x00, 0x00 };
        char *add = build_code(ag, BV(0xe0990, 0xe0990, 0x15cd80), BV(add_sig, add_sig, add_sig26), BV(sizeof add_sig, sizeof add_sig, sizeof add_sig26));
        if (add) g_add_place = (AddPlace)install_detour(add, BV(19, 19, 18), add_place_hook);
        mp_log("places: %s", g_add_place ? "counted by where they come from" : "could not be traced");
    }
    // the dig: the same first bytes in beta26 (+0x154f70)
    char *dig = build_code(ag, BV(0xdbc20, 0xdbc20, 0x154f70), sig, sizeof sig);
    if (!dig) { mp_log("trench: unknown game build, each game digs for itself"); g_enabled = false; return; }
    g_dig = (Dig)install_detour(dig, 14, dig_hook);
    static const unsigned char gen_sig[] = { 0x44, 0x89, 0x4c, 0x24, 0x20, 0x48, 0x89, 0x4c, 0x24, 0x08, 0x55, 0x53, 0x57, 0x41, 0x55, 0x41, 0x57,
                                             0x48, 0x8d, 0xac, 0x24 };
    // beta26 +0x155e50: mov [rsp+20h],r9d; mov [rsp+8],rcx; push rbp/rbx/rsi/rdi/r13 | lea rbp,[rsp-1130h]
    static const unsigned char gen_sig26[] = { 0x44, 0x89, 0x4c, 0x24, 0x20, 0x48, 0x89, 0x4c, 0x24, 0x08, 0x55, 0x53, 0x56, 0x57, 0x41, 0x55,
                                               0x48, 0x8d, 0xac, 0x24, 0xd0, 0xee, 0xff, 0xff };
    char *gen = build_code(ag, BV(0xdc9a0, 0xdc9a0, 0x155e50), BV(gen_sig, gen_sig, gen_sig26), BV(sizeof gen_sig, sizeof gen_sig, sizeof gen_sig26));
    if (g_dig && g_enabled && gen) g_gen = (Gen)install_detour(gen, BV(17, 17, 16), gen_hook);
    static const unsigned char crater_sig[] = { 0x4c, 0x89, 0x44, 0x24, 0x18, 0x48, 0x89, 0x4c, 0x24, 0x08, 0x55, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x57,
                                                0x48, 0x8d, 0xac, 0x24 };
    // beta26 +0x157d30: mov [rsp+20h],r9d; mov [rsp+18h],r8; mov [rsp+8],rcx; push rbp/rdi/r13/r15 | lea rbp,[rsp-958h]
    static const unsigned char crater_sig26[] = { 0x44, 0x89, 0x4c, 0x24, 0x20, 0x4c, 0x89, 0x44, 0x24, 0x18, 0x48, 0x89, 0x4c, 0x24, 0x08, 0x55, 0x57,
                                                  0x41, 0x55, 0x41, 0x57, 0x48, 0x8d, 0xac, 0x24, 0xa8, 0xf6, 0xff, 0xff };
    char *crater = build_code(ag, BV(0xde720, 0xde720, 0x157d30), BV(crater_sig, crater_sig, crater_sig26),
                              BV(sizeof crater_sig, sizeof crater_sig, sizeof crater_sig26));
    if (GetEnvironmentVariableA("MP_SYNC_CRATERS", v, sizeof v)) g_crater_sync = atoi(v) != 0;
    if (g_enabled && crater) g_crater = (Crater)install_detour(crater, BV(18, 18, 21), crater_hook);
    mp_log("crater: %s", !g_crater ? "each game makes its own" : g_crater_sync ? "the ground is the host's: its craters are sent and made the same here"
                                                                             : "each game makes its own (MP_SYNC_CRATERS=0)");
    mp_log("trench: %s", !g_dig ? "could not be hooked" : g_enabled ? "a squad's trenches are its owner's: dug there, sent, and dug the same here"
                                                                     : "each game digs for itself (MP_SYNC_TRENCH=0); digs are logged");
    if (!g_dig) g_enabled = false;
}

void trench_report(void)
{
    if (g_stat_crater_own || g_stat_crater_dropped || g_stat_crater_applied || g_stat_crater_other_thread || g_stat_crater_had)
        mp_log("crater: %llu made here as the host, %llu of the host's made here (%llu more this game had made itself already), %llu of this game's own dropped, %llu from another thread left alone",
               g_stat_crater_own, g_stat_crater_applied, g_stat_crater_had, g_stat_crater_dropped, g_stat_crater_other_thread);
    g_stat_crater_own = g_stat_crater_dropped = g_stat_crater_applied = g_stat_crater_other_thread = g_stat_crater_had = 0;
    if (g_stat_stamp_sent || g_stat_stamp_made || g_stat_stamp_had || g_stat_stamp_twice)
        mp_log("trench: outside any dig: %llu stamped here told to the other game, %llu of the other game's stamped here, %llu of them this game had already; %llu of this game's own left out (made on the other game's word before)",
               g_stat_stamp_sent, g_stat_stamp_made, g_stat_stamp_had, g_stat_stamp_twice);
    g_stat_stamp_sent = g_stat_stamp_made = g_stat_stamp_had = g_stat_stamp_twice = 0;
    if (!g_stat_own && !g_stat_dropped && !g_stat_applied && !g_stat_missing) return;
    mp_log("trench: %llu squads of this player dug in and sent, %llu of the other player dug in here from his values, %llu local digs for his squads dropped, %llu for squads this game does not have",
           g_stat_own, g_stat_applied, g_stat_dropped, g_stat_missing);
    mp_log("trench: of his digs %llu stamped a trench here as in his game (%llu of them where this game's own dig would not have), %llu were kept from stamping one his game did not; %llu trenches stamped outside any dig so far",
           g_stat_stamped + g_stat_stamp_added, g_stat_stamp_added, g_stat_stamp_held, g_stat_stamp_other);
    g_stat_own = g_stat_dropped = g_stat_applied = g_stat_missing = 0;
    g_stat_stamped = g_stat_stamp_added = g_stat_stamp_held = 0;
}
