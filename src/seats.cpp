// Who rides which vehicle (mpmod2). A soldier's seat is his vehicle (soldier +0x2388, NULL on foot; a second copy at
// +0x7d0) and his crew station in it (+0x1da4, -1 none); the vehicle's stations (vehicle +0x1900, 320-byte records,
// count +0x190c) hold their soldier at +0xc8. The game changes them through two calls (adv_physic; the Mius Framework
// research, kb/engine/mounting, checked in IDA in all three builds):
//   board   soldier slot 195 (beta26 197): his three fields, then the vehicle's slot that seats him (evicting whoever
//           held the station) - beta16 +0x6f9f0, public +0x6f980, beta26 +0xbc880
//   unseat  vehicle slot 185 (beta26 187): the station is emptied if it holds him - beta16 +0x85680, public
//           +0x85610, beta26 +0xe3930
// The game whose simulation decides a soldier (session_owns) sends his seat whenever it changes, and all of them again
// on a new connection: ShotRec kind 16, up to 7 seats a record (launch: u64 soldier, u64 vehicle or 0, int station).
// The other game puts each soldier there with the same two calls, on its game thread, and again whenever its own
// simulation or a loaded state has put him elsewhere.
#include "common.h"
#include "protocol.h"
#include <string.h>
#include <algorithm>
#include <unordered_map>
#include <vector>

#define S_VEHICLE   BF(0x2388, 0x2b90)
#define S_VEHICLE2  BF(0x7d0, 0x800)
#define S_STATION   BF(0x1da4, 0x2204)
#define S_LEAVING   BF(0x2297, 0x2a63)        // a leave is pending (+1: at once); the in-vehicle update carries it out
#define V_STATIONS  BF(0x1900, 0x1e40)
#define V_COUNT     BF(0x190c, 0x1e4c)
#define V_STATION   BF(0x140, 0x170)
#define ST_SOLDIER  BF(0xc8, 0xe8)
#define BOARD_SLOT  BF(195, 197)
#define UNSEAT_SLOT BF(185, 187)
#define BOARD_FN    BV(0x6f9f0, 0x6f980, 0xbc880)
#define UNSEAT_FN   BV(0x85680, 0x85610, 0xe3930)

namespace {
enum { SEATS = 16, PER_REC = 7 };
struct Seat { u64 vehicle; int station; bool operator!=(const Seat &o) const { return vehicle != o.vehicle || station != o.station; } };
typedef void (*BoardFn)(Object *soldier, Object *vehicle, u32 station);
typedef void (*UnseatFn)(Object *vehicle, Object *soldier, u32 station);
}
static std::unordered_map<u64, Seat> g_sent, g_wanted;
static DWORD g_last_scan, g_last_check;
static char *g_physic;
static bool g_enabled = true, g_checked, g_ok;
static u64 g_stat_sent, g_stat_board, g_stat_leave, g_stat_evicted, g_stat_failed;

static bool ready(void)
{
    if (!g_checked) {
        g_checked = true;
        char v[8];
        if (GetEnvironmentVariableA("MP_SYNC_SEATS", v, sizeof v)) g_enabled = atoi(v) != 0;
        g_physic = build_module("adv_physic.codelib");
        g_ok = g_physic != 0;
        mp_log("seats: %s", !g_enabled ? "each game seats its own soldiers (MP_SYNC_SEATS=0)" : g_ok ?
               "who rides which vehicle is the deciding game's: sent, and seated the same way here" : "unknown game build, each game seats its own soldiers");
    }
    return g_enabled && g_ok && session_connected() && session_versus_or_coop() && session_simulating();
}

static bool is_soldier(Object *o) { return o && !strcmp(o->iface, "i_human"); }
static bool is_vehicle(Object *o) { return o && !strcmp(o->iface, "i_techn"); }

// The seat a soldier has in this game (his vehicle as its number).
static bool seat_of(Object *o, Seat *s)
{
    char *u = (char *)o;
    if (!readable(u + S_STATION, 4) || !readable(u + S_VEHICLE, 8)) return false;
    Object *v = *(Object **)(u + S_VEHICLE);
    s->station = *(int *)(u + S_STATION);
    s->vehicle = 0;
    if (v && !session_unit_key(v, &s->vehicle)) return false;
    if (!v) s->station = -1;
    return true;
}

// The game's own calls, each only when its vtable entry is this build's function.
static UnseatFn unseat_fn(Object *v)
{
    void **vt = *(void ***)v;
    return readable(vt + UNSEAT_SLOT, 8) && vt[UNSEAT_SLOT] == g_physic + UNSEAT_FN ? (UnseatFn)vt[UNSEAT_SLOT] : 0;
}
static BoardFn board_fn(Object *o)
{
    void **vt = *(void ***)o;
    return readable(vt + BOARD_SLOT, 8) && vt[BOARD_SLOT] == g_physic + BOARD_FN ? (BoardFn)vt[BOARD_SLOT] : 0;
}

static bool unseat(Object *o, Object *v, int station)
{
    UnseatFn f = unseat_fn(v);
    if (!f) return false;
    char *u = (char *)o;
    if (station >= 0) f(v, o, (u32)station);
    if (*(Object **)(u + S_VEHICLE) == v) {
        *(Object **)(u + S_VEHICLE) = 0;
        *(Object **)(u + S_VEHICLE2) = 0;
        *(int *)(u + S_STATION) = -1;
        u[S_LEAVING] = 0;
        u[S_LEAVING + 1] = 0;
    }
    return true;
}

static bool board(Object *o, Object *v, int station)
{
    char *vb = (char *)v;
    if (station < 0 || !readable(vb + V_COUNT, 4) || (u32)station >= *(u32 *)(vb + V_COUNT)) return false;
    BoardFn f = board_fn(o);
    char *stations = *(char **)(vb + V_STATIONS);
    if (!f || !readable(stations + (size_t)station * V_STATION, V_STATION)) return false;
    Object *other = *(Object **)(stations + (size_t)station * V_STATION + ST_SOLDIER);
    if (other && other != o) {          // whoever holds the station here: his own seat follows in its own entry
        if (!is_soldier(other) || !unseat(other, v, station)) return false;
        g_stat_evicted++;
    }
    f(o, v, (u32)station);
    return true;
}

// Puts a soldier where the deciding game has him. 0: there already, 1: moved, -1: could not.
static int seat_to(Object *o, const Seat &want)
{
    Seat now;
    if (!seat_of(o, &now)) return -1;
    if (!(now != want)) return 0;
    char *u = (char *)o;
    Object *cur = *(Object **)(u + S_VEHICLE);
    Object *v = want.vehicle ? session_unit(want.vehicle) : 0;
    if (want.vehicle && !is_vehicle(v)) return -1;
    __try {
        if (cur) {
            if (!unseat(o, cur, now.station)) return -1;
            g_stat_leave++;
        }
        if (v) {
            if (!board(o, v, want.station)) return -1;
            g_stat_board++;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    return 1;
}

static void send_batch(std::vector<std::pair<u64, Seat>> &batch)
{
    for (size_t at = 0; at < batch.size(); at += PER_REC) {
        ShotRec r = {};
        r.kind = SEATS;
        size_t n = batch.size() - at < PER_REC ? batch.size() - at : PER_REC;
        r.arg6 = (u32)n;
        for (size_t i = 0; i < n; i++) {
            unsigned char *e = r.launch + i * 20;
            memcpy(e, &batch[at + i].first, 8);
            memcpy(e + 8, &batch[at + i].second.vehicle, 8);
            memcpy(e + 16, &batch[at + i].second.station, 4);
        }
        shots_queue(r);
        g_stat_sent += n;
    }
    batch.clear();
}

// ---- tests: "seats" (who rides where here, as a count and a hash both games can compare), "unseat <n>" (the n-th
// soldier seated here, by number, out of his seat), "reseat" (him back in it). Carried out on the game thread.
static volatile LONG g_test_list, g_test_move;
static int g_test_arg;
static u64 g_taken;
static Seat g_taken_seat;

void seats_test_list(void) { InterlockedExchange(&g_test_list, 1); }
void seats_test_move(int index) { g_test_arg = index; InterlockedExchange(&g_test_move, 1); }

static void soldiers_by_number(ObjMan *om, std::vector<std::pair<u64, Object *>> &out)
{
    u32 count;
    Object **all = all_objects(om, &count);
    for (u32 i = 0; i < count; i++) {
        u64 key;
        if (is_soldier(all[i]) && session_unit_key(all[i], &key)) out.push_back({ key, all[i] });
    }
    std::sort(out.begin(), out.end(), [](const std::pair<u64, Object *> &a, const std::pair<u64, Object *> &b) { return a.first < b.first; });
}

static void test_now(ObjMan *om)
{
    if (InterlockedExchange(&g_test_list, 0)) {
        std::vector<std::pair<u64, Object *>> all;
        soldiers_by_number(om, all);
        std::vector<unsigned char> recs;
        u32 riding = 0, n = 0;
        for (auto &kv : all) {
            Seat s;
            if (!seat_of(kv.second, &s)) continue;
            n++;
            if (s.vehicle) riding++;
            unsigned char e[20];
            memcpy(e, &kv.first, 8);
            memcpy(e + 8, &s.vehicle, 8);
            memcpy(e + 16, &s.station, 4);
            recs.insert(recs.end(), e, e + 20);
        }
        mp_log("test: seats - %u of %u soldiers ride here, hash %08x", riding, n, recs.empty() ? 0 : fnv(recs.data(), recs.size()));
    }
    if (InterlockedExchange(&g_test_move, 0)) {
        if (g_test_arg >= 0) {
            std::vector<std::pair<u64, Object *>> all;
            soldiers_by_number(om, all);
            int at = 0;
            for (auto &kv : all) {
                Seat s;
                if (!seat_of(kv.second, &s) || !s.vehicle || s.station < 0 || at++ < g_test_arg) continue;
                Seat foot = { 0, -1 };
                int r = seat_to(kv.second, foot);
                g_taken = kv.first;
                g_taken_seat = s;
                mp_log("test: seat - soldier %08x taken out of vehicle %08x seat %d: %s", (u32)kv.first, (u32)s.vehicle, s.station, r > 0 ? "done" : "FAILED");
                return;
            }
            mp_log("test: seat - no seated soldier number %d here", g_test_arg);
        } else if (g_taken) {
            Object *o = session_unit(g_taken);
            int r = is_soldier(o) ? seat_to(o, g_taken_seat) : -1;
            mp_log("test: seat - soldier %08x put back in vehicle %08x seat %d: %s", (u32)g_taken, (u32)g_taken_seat.vehicle, g_taken_seat.station,
                   r > 0 ? "done" : r == 0 ? "he was there" : "FAILED");
        }
    }
}

// Every simulation step: the deciding game sends changed seats (four times a second); the other game seats the
// soldiers as they should be (at once for news, once a second for all - a loaded state can have moved them),
// once its battle is set up (apply).
void seats_sim_step(ObjMan *om, bool apply)
{
    if (!ready()) return;
    DWORD now = GetTickCount();
    if (g_test_list || g_test_move) test_now(om);
    if (now - g_last_scan >= 250) {
        g_last_scan = now;
        std::vector<std::pair<u64, Seat>> batch;
        u32 count;
        Object **all = all_objects(om, &count);
        for (u32 i = 0; i < count; i++) {
            Object *o = all[i];
            u64 key;
            Seat s;
            if (!is_soldier(o) || !session_owns(o) || !session_unit_key(o, &key) || !seat_of(o, &s)) continue;
            auto it = g_sent.find(key);
            if (it != g_sent.end() && !(it->second != s)) continue;
            g_sent[key] = s;
            batch.push_back({ key, s });
        }
        if (!batch.empty()) send_batch(batch);
    }
    if (!apply) return;
    bool all_now = now - g_last_check >= 1000;
    if (all_now) g_last_check = now;
    for (auto &w : g_wanted) {
        Object *o = session_unit(w.first);
        if (!is_soldier(o) || session_owns(o)) continue;
        Seat now_seat = { 0, -1 };
        bool known = seat_of(o, &now_seat);
        if (!all_now && known && !(now_seat != w.second)) continue;
        int r = seat_to(o, w.second);
        if (r < 0) g_stat_failed++;
        static int logged;
        if (r != 0 && logged < 24) {
            logged++;
            mp_log("seats: %s from %s %08x seat %d to %08x seat %d as the other game has him%s", o->full, now_seat.vehicle ? "vehicle" : "foot",
                   (u32)now_seat.vehicle, now_seat.station, (u32)w.second.vehicle, w.second.station, r < 0 ? " - COULD NOT" : "");
        }
    }
}

void seats_received(const ShotRec &r)
{
    u32 n = r.arg6 > PER_REC ? PER_REC : r.arg6;
    for (u32 i = 0; i < n; i++) {
        const unsigned char *e = r.launch + i * 20;
        u64 key;
        Seat s;
        memcpy(&key, e, 8);
        memcpy(&s.vehicle, e + 8, 8);
        memcpy(&s.station, e + 16, 4);
        g_wanted[key] = s;
    }
    g_last_check = 0;           // look at once
}

// A new connection: every seat is sent again; what the other game asked for stays until it says otherwise.
void seats_reset(void) { g_sent.clear(); }

void seats_report(void)
{
    if (g_stat_sent || g_stat_board || g_stat_leave || g_stat_failed)
        mp_log("seats: 5 s: %llu seats sent; here %llu soldiers seated and %llu taken out as the other game has them (%llu others "
               "made room), %llu could not be", g_stat_sent, g_stat_board, g_stat_leave, g_stat_evicted, g_stat_failed);
    g_stat_sent = g_stat_board = g_stat_leave = g_stat_evicted = g_stat_failed = 0;
}
