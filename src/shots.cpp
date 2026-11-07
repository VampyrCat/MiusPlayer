// Shots: every shell, bullet, grenade and flare in a battle is created by one engine function, the
// "launch" of the shell manager (i_shellphys, virtual slot 3, basic_physic+0x18430; beta26 +0x2a030). It makes the
// projectile, its trail and tracer, the muzzle flash, the sound of the shot and the report the computer
// opponents hear. A unit's own update calls it when the unit fires.
//
// Each game simulates every unit, including the ones the other player commands, so left alone each game
// fires those units' weapons by itself: at slightly different moments, in slightly different directions,
// and - when a newly loaded state says the weapon has already fired - not at all. Those were the shots
// that looked different on the two PCs or were missing on one of them.
//
// With another player connected the launch is therefore the owner's alone:
//   - a launch by a unit this game owns happens as usual and is sent to the other game;
//   - a launch this game's own simulation makes for a unit the other game owns is dropped;
//   - a launch received from the other game is carried out here with exactly the owner's values.
// The launch record holds pointers to the ammunition and weapon descriptions and to the firing unit. They
// travel as the ids the engine's own savegame code uses for them (object manager, virtual slots 56/57) and
// as unit numbers, and are checked against the local data before use.
//
// A launch without a firing unit is off-map fire: the divisions manager's fire mission worker
// (adv_game+0x42d40) launches the rounds of a battery that is not on the map, with the side's code in the
// launch record (+0x68). Such a launch belongs to the game that decides for that side (session_owns_side):
// the joined player's game for his own artillery, the host's for the rest.
//
// Two other things happen at a place without a flight, and are handled the same way - made by one game,
// sent, carried out by the other with the same values, and dropped when the other game's own simulation
// makes them:
//   - an explosion (i_shellphys slot 4, basic_physic+0x19260; beta26 +0x2b4c0): the engine uses it when a
//     vehicle's crew blows their vehicle up (adv_physic+0x9b520, "DETONATE"; beta26 +0x111a30, which also
//     sets off the later charges "DET4"); it belongs to the vehicle's owner;
//   - a flare (adv_game+0x44c40, divisions slot 45): fired by a squad at night or by an illumination
//     mission; it belongs to the side it is fired for. (beta26: slot 46 is a wrapper of +0x660e0, which takes
//     a 7th argument - 24 bytes of the flare's own numbers or 0 - and which the illumination mission and the
//     hand flares call directly; that one is hooked.)
//
// MP_SYNC_SHOTS=0 switches all of this off; MP_LOG_SHOTS=1 writes every launch to out\mp_shots_<pid>.txt.
#include "common.h"
#include "build.h"
#include "protocol.h"
#include "detour.h"
#include <intrin.h>
#include <share.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <deque>
#include <vector>

typedef void (*Launch)(char *sp, char *rec, u64 flags, float *pos, void *obj5, u32 arg6);
typedef void (*Explode)(char *sp, char *rec);
typedef void (*Detonate)(char *vehicle, float dt);
// (the 7th argument is beta26's only; the 2024 builds' function takes six and never sees it)
typedef void (*Flare)(char *div, float *pos, u32 kind, float *dir, u32 side, int arg6, const void *params);
typedef u32 (*FireMission)(void *div, u32 squad, u64 flags, u32 kind, u32 rounds, float arg6);
static Launch g_launch;
static Explode g_explode;
static Detonate g_detonate;
static Flare g_flare;
static ObjMan *g_om;
static char *g_shellphys, *g_divisions;
static char *g_blowing_up;              // the vehicle whose crew is blowing it up right now (explosion's owner)
static DWORD g_main_thread;
static bool g_injecting, g_enabled = true;
static FILE *g_file;
static u32 g_seq, g_epoch, g_last_seq_in, g_epoch_in, g_dumped;
static int g_test_rounds, g_test_kind, g_test_blowup, g_test_flares;
static u64 g_stat_own, g_stat_dropped, g_stat_injected, g_stat_refused, g_stat_free, g_stat_sent_bytes, g_stat_repeats;
static u64 g_stat_offmap, g_stat_x_own, g_stat_x_dropped, g_stat_x_in, g_stat_f_own, g_stat_f_dropped, g_stat_f_in;
struct ShotPending { ShotRec rec; DWORD sent; };
static std::deque<ShotPending> g_out;               // launched here, not yet acknowledged by the other game
static std::vector<ShotRec> g_in;               // received, carried out before the next simulation step

// launch record: pointers at +0x38 (ammunition), +0x40 (weapon), +0x48 (firing object), +0x50 (object),
// +0x60 (description); +0x68 the side's code
enum { L_AMMO = 0x38, L_WEAPON = 0x40, L_SHOOTER = 0x48, L_OBJ2 = 0x50, L_DESC3 = 0x60, L_SIDE = 0x68, L_SIZE = 0x90 };
// explosion record: place at +0, two description pointers at +0x40 / +0x48 (the only things slot 4 reads),
// 0x58 bytes as the engine builds it
enum { X_DESC1 = 0x40, X_DESC2 = 0x48, X_SIZE = 0x58 };
// ShotRec kinds of this file: 0 launch, 1 explosion, 9 flare
enum { SHOT_LAUNCH = 0, SHOT_EXPLOSION = 1, SHOT_FLARE = 9 };
// beta26 flare's own numbers (its 7th argument): 6 x 4 bytes, read at +0..+0x14 by adv_game+0x660e0
enum { FLARE_PARAMS = 24 };

static u32 desc_id(void *p) { return p ? ((u32 (*)(ObjMan *, void *))g_om->vtbl[OM_SLOT_DESC_ID])(g_om, p) : 0xffffffffu; }
static void *desc_ptr(u32 id) { return id == 0xffffffffu ? 0 : ((void *(*)(ObjMan *, u32))g_om->vtbl[OM_SLOT_DESC_PTR])(g_om, id); }

static bool is_object(void *p)
{
    if (!readable(p, sizeof(Object))) return false;
    Object *o = (Object *)p;
    return o->objman == g_om && readable(o->iface, 4);
}

static u64 object_key(void *p)
{
    u64 key = 0;
    if (p && is_object(p)) session_unit_key((Object *)p, &key);
    return key;
}

// what identifies an ammunition description besides its id: its first two numbers and its type code
static void ammo_check(void *ammo, float *two, u32 *code)
{
    two[0] = two[1] = 0;
    *code = 0;
    if (!readable(ammo, 0xc8)) return;
    memcpy(two, ammo, 8);
    memcpy(code, (char *)ammo + 0xc4, 4);
}

static const char *name_of(void *p)
{
    return !p ? "-" : is_object(p) ? ((Object *)p)->full : "?";
}

static void log_launch(const char *who, char kind, const char *rec, u64 flags, void *obj5, void *caller)
{
    if (!g_file) return;
    const float *f = (const float *)rec;
    void *ammo = *(void **)(rec + L_AMMO), *weapon = *(void **)(rec + L_WEAPON), *shooter = *(void **)(rec + L_SHOOTER);
    HMODULE m = 0;
    char mod[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)caller, &m) && m)
        GetModuleFileNameA(m, mod, MAX_PATH);
    const char *base = strrchr(mod, '\\');
    u32 aflags = readable(ammo, 0xc8) ? *(u32 *)((char *)ammo + 0xc0) : 0, acode = readable(ammo, 0xc8) ? *(u32 *)((char *)ammo + 0xc4) : 0;
    fprintf(g_file, "%c %llu %s %s ammo=%u weapon=%u aflags=%08x acode=%08x flags=%llx pos=%.2f,%.2f,%.2f dir=%.4f,%.4f,%.4f speed=%.1f n=%u obj2=%s obj5=%s from=%s+0x%llx\n",
            kind, session_battle_ms(), who, name_of(shooter), desc_id(ammo), desc_id(weapon), aflags, acode, flags,
            f[0], f[1], f[2], f[4], f[5], f[6], kind == 'L' ? f[0xc] : 0.0f, kind == 'L' ? *(u32 *)(rec + 0x70) : 0,
            kind == 'L' ? name_of(*(void **)(rec + L_OBJ2)) : "-", name_of(obj5), base ? base + 1 : mod, m ? (u64)((char *)caller - (char *)m) : 0);
    fflush(g_file);
    if (g_dumped < 6 && kind == 'L') {
        g_dumped++;
        char line[3 * L_SIZE + 8];
        size_t w = 0;
        for (int i = 0; i < L_SIZE; i++) w += sprintf_s(line + w, sizeof line - w, "%02x%s", (unsigned char)rec[i], (i & 7) == 7 ? " " : "");
        mp_log("shots: launch record of %s: %s", name_of(shooter), line);
    }
}

static bool replicating(void)
{
    return g_enabled && session_connected() && session_role() != ROLE_NONE;
}

// Whose launch is it? A unit's launch belongs to the game that owns the unit; one with no firing unit
// behind it (off-map fire) to the game that decides for the side named in the record.
static bool mine(void *shooter, u32 side)
{
    if (shooter && is_object(shooter)) return session_owns((Object *)shooter);
    return session_owns_side(side);
}

static void queue_shot(const char *rec, u64 flags, const float *pos, void *obj5, u32 arg6)
{
    ShotRec s = {};
    if (!g_epoch) g_epoch = (GetTickCount() ^ (GetCurrentProcessId() << 16)) | 1;
    s.seq = ++g_seq;
    s.kind = SHOT_LAUNCH;
    s.flags = (unsigned char)flags;
    s.has_pos = pos != 0;
    void *ammo = *(void **)(rec + L_AMMO);
    s.ammo = desc_id(ammo);
    s.weapon = desc_id(*(void **)(rec + L_WEAPON));
    ammo_check(ammo, s.ammo_check, &s.ammo_code);
    s.desc3 = desc_id(*(void **)(rec + L_DESC3));
    s.shooter = object_key(*(void **)(rec + L_SHOOTER));
    s.obj2 = object_key(*(void **)(rec + L_OBJ2));
    s.obj5 = object_key(obj5);
    s.arg6 = arg6;
    if (pos) memcpy(s.pos, pos, 16);
    memcpy(s.launch, rec, L_SIZE);
    memset(s.launch + L_AMMO, 0, 0x20);             // the four pointers
    memset(s.launch + L_DESC3, 0, 8);
    g_out.push_back({ s, 0 });
}

// An explosion: its place and its two descriptions (ammo / weapon carry their ids, launch the record with
// the pointers blanked); shooter = the vehicle it came from, for the log.
static void queue_explosion(const char *rec, void *vehicle)
{
    ShotRec s = {};
    if (!g_epoch) g_epoch = (GetTickCount() ^ (GetCurrentProcessId() << 16)) | 1;
    s.seq = ++g_seq;
    s.kind = SHOT_EXPLOSION;
    void *d1 = *(void **)(rec + X_DESC1), *d2 = *(void **)(rec + X_DESC2);
    s.weapon = desc_id(d1);
    s.ammo = desc_id(d2);
    s.desc3 = 0xffffffffu;
    ammo_check(d2, s.ammo_check, &s.ammo_code);
    s.shooter = object_key(vehicle);
    memcpy(s.launch, rec, X_SIZE);
    memset(s.launch + X_DESC1, 0, X_SIZE - X_DESC1);
    g_out.push_back({ s, 0 });
}

// A flare: the engine's arguments as they were (place, kind, direction, side, last argument; beta26: the
// flare's own numbers when it was given them, launch[28] says so).
static void queue_flare(const float *pos, u32 kind, const float *dir, u32 side, int arg6, const void *params)
{
    ShotRec s = {};
    if (!g_epoch) g_epoch = (GetTickCount() ^ (GetCurrentProcessId() << 16)) | 1;
    s.seq = ++g_seq;
    s.kind = SHOT_FLARE;
    s.ammo = s.weapon = s.desc3 = 0xffffffffu;
    memcpy(s.pos, pos, 12);
    memcpy(s.launch, dir, 16);
    memcpy(s.launch + 16, &kind, 4);
    memcpy(s.launch + 20, &side, 4);
    memcpy(s.launch + 24, &arg6, 4);
    if (params) { s.launch[28] = 1; memcpy(s.launch + 32, params, FLARE_PARAMS); }
    g_out.push_back({ s, 0 });
}

// Another kind of record for the same channel (trench.cpp).
void shots_queue(ShotRec &s)
{
    if (!g_epoch) g_epoch = (GetTickCount() ^ (GetCurrentProcessId() << 16)) | 1;
    s.seq = ++g_seq;
    g_out.push_back({ s, 0 });
}

static void launch_hook(char *sp, char *rec, u64 flags, float *pos, void *obj5, u32 arg6)
{
    // bit 1: asked for from another thread; the shell manager queues it and launches it later through
    // this same function, on the simulation's thread
    if ((flags & 2) || g_injecting || GetCurrentThreadId() != g_main_thread || !readable(rec, L_SIZE)) {
        g_launch(sp, rec, flags, pos, obj5, arg6);
        return;
    }
    g_shellphys = sp;
    void *shooter = *(void **)(rec + L_SHOOTER);
    bool repl = replicating(), own = !repl || mine(shooter, *(u32 *)(rec + L_SIDE));
    bool offmap = !shooter;
    log_launch(!repl ? "free" : own ? (offmap ? "own-offmap" : "own") : (offmap ? "dropped-offmap" : "dropped"), 'L', rec, flags, obj5, _ReturnAddress());
    if (!repl) { g_stat_free++; g_launch(sp, rec, flags, pos, obj5, arg6); return; }
    if (!own) { g_stat_dropped++; return; }
    g_stat_own++;
    if (offmap) g_stat_offmap++;
    g_launch(sp, rec, flags, pos, obj5, arg6);
    if (*(void **)(rec + L_AMMO) && *(void **)(rec + L_WEAPON)) queue_shot(rec, flags, pos, obj5, arg6);
}

// The explosion record as a launch record, for the log (place, then the two descriptions where a launch
// has its ammunition and weapon).
static void log_explosion(const char *who, const char *rec, void *caller)
{
    if (!g_file) return;
    char fake[L_SIZE] = {};
    memcpy(fake, rec, 0x30);
    memcpy(fake + L_AMMO, rec + X_DESC2, 8);
    memcpy(fake + L_WEAPON, rec + X_DESC1, 8);
    *(char **)(fake + L_SHOOTER) = g_blowing_up;
    log_launch(who, 'X', fake, 0, 0, caller);
}

// Virtual slot 4 (basic_physic+0x19260): an ammunition going off at a place, without a flight. It belongs
// to the vehicle being blown up when that is what it is, to the host otherwise.
static void explode_hook(char *sp, char *rec)
{
    if (g_injecting || GetCurrentThreadId() != g_main_thread || !readable(rec, X_SIZE)) { g_explode(sp, rec); return; }
    g_shellphys = sp;
    void *vehicle = g_blowing_up;
    bool repl = replicating() && g_enabled;
    bool own = !repl || (vehicle && is_object(vehicle) ? session_owns((Object *)vehicle) : session_role() == ROLE_HOST);
    log_explosion(!repl ? "free" : own ? "own" : "dropped", rec, _ReturnAddress());
    if (!own) { g_stat_x_dropped++; return; }
    g_explode(sp, rec);
    if (repl) {
        g_stat_x_own++;
        if (*(void **)(rec + X_DESC1) || *(void **)(rec + X_DESC2)) queue_explosion(rec, vehicle);
    }
}

// adv_physic+0x9b520(vehicle, dt): a vehicle's crew counting down to blowing it up, and doing it. Only
// marks whose explosion the slot 4 call inside it is.
static void detonate_hook(char *vehicle, float dt)
{
    char *outer = g_blowing_up;
    g_blowing_up = vehicle;
    g_detonate(vehicle, dt);
    g_blowing_up = outer;
}

// adv_game+0x44c40(divisions, place, kind, direction, side, arg) (beta26 +0x660e0, and the flare's own
// numbers or 0): flares go up. The side's game decides.
static void flare_hook(char *div, float *pos, u32 kind, float *dir, u32 side, int arg6, const void *params)
{
    g_divisions = div;
    if (g_bi != BUILD_BETA26) params = 0;           // (no 7th argument there: whatever is on the stack)
    if (g_injecting || GetCurrentThreadId() != g_main_thread || !replicating() || !g_enabled || !readable(pos, 12) || !readable(dir, 16) ||
        (params && !readable(params, FLARE_PARAMS))) {
        g_flare(div, pos, kind, dir, side, arg6, params);
        return;
    }
    if (!session_owns_side(side)) {
        g_stat_f_dropped++;
        if (g_file) { fprintf(g_file, "F %llu dropped side=%08x kind=%u pos=%.2f,%.2f,%.2f\n", session_battle_ms(), side, kind, pos[0], pos[1], pos[2]); fflush(g_file); }
        return;
    }
    g_flare(div, pos, kind, dir, side, arg6, params);
    g_stat_f_own++;
    if (g_file) { fprintf(g_file, "F %llu own side=%08x kind=%u pos=%.2f,%.2f,%.2f\n", session_battle_ms(), side, kind, pos[0], pos[1], pos[2]); fflush(g_file); }
    queue_flare(pos, kind, dir, side, arg6, params);
}

void shots_install(ObjMan *om)
{
    // (each signature is whole instructions, none position dependent: the detour copies all of it)
    static const unsigned char launch_sig[] = { 0x48, 0x89, 0x54, 0x24, 0x10, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56,
                                                0x48, 0x8d, 0xac, 0x24, 0xe8, 0xfd, 0xff, 0xff };
    // beta26 +0x2a030: push rbp/rbx/rsi/rdi/r14/r15; lea rbp,[rsp-268h]; sub rsp,368h
    static const unsigned char launch_sig26[] = { 0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0xac, 0x24, 0x98,
                                                  0xfd, 0xff, 0xff, 0x48, 0x81, 0xec, 0x68, 0x03, 0x00, 0x00 };
    static const unsigned char explode_sig[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x81, 0xec, 0x70, 0x02, 0x00, 0x00, 0x48, 0x8b, 0xf9 };
    // beta26 +0x2b4c0: the same with sub rsp,290h
    static const unsigned char explode_sig26[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x81, 0xec, 0x90, 0x02, 0x00, 0x00, 0x48, 0x8b, 0xf9 };
    static bool done;
    if (done) return;
    if (!build_module("basic_physic.codelib")) return;
    done = true;
    g_om = om;
    g_main_thread = GetCurrentThreadId();
    char v[16];
    if (GetEnvironmentVariableA("MP_SYNC_SHOTS", v, sizeof v)) g_enabled = atoi(v) != 0;
    if (GetEnvironmentVariableA("MP_LOG_SHOTS", v, sizeof v) && atoi(v)) {
        char path[MAX_PATH];
        sprintf_s(path, "%s\\mp_shots_%lu.txt", g_out_dir, GetCurrentProcessId());
        g_file = _fsopen(path, "w", _SH_DENYNO);
    }
    if (!g_enabled && !g_file) { mp_log("shots: each game fires for itself (MP_SYNC_SHOTS=0)"); return; }
    const unsigned char *lsig = BV(launch_sig, launch_sig, launch_sig26), *xsig = BV(explode_sig, explode_sig, explode_sig26);
    size_t lsig_n = BV(sizeof launch_sig, sizeof launch_sig, sizeof launch_sig26), xsig_n = sizeof explode_sig;
    char *launch = build_code("basic_physic.codelib", BV(0x18430, 0x18430, 0x2a030), lsig, lsig_n);
    char *explode = build_code("basic_physic.codelib", BV(0x19260, 0x19260, 0x2b4c0), xsig, xsig_n);
    if (!launch) { mp_log("shots: unknown game build, each game fires for itself"); g_enabled = false; return; }
    g_launch = (Launch)install_detour(launch, lsig_n, launch_hook);
    if (explode) g_explode = (Explode)install_detour(explode, xsig_n, explode_hook);
    // the crew blowing up their vehicle: adv_physic+0x9b520 (mov rax,rsp; mov [rax+10h],rbx; push rbp/rsi/rdi; sub rsp,0e0h)
    static const unsigned char detonate_sig[] = { 0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x10, 0x55, 0x56, 0x57, 0x48, 0x81, 0xec, 0xe0, 0x00, 0x00, 0x00 };
    // beta26 +0x111a30 (mov rax,rsp; push rbp/rbx; lea rbp,[rax-5fh]; sub rsp,0d8h; cmp byte [rcx+2eb4h],0 - the charges set)
    static const unsigned char detonate_sig26[] = { 0x48, 0x8b, 0xc4, 0x55, 0x53, 0x48, 0x8d, 0x68, 0xa1, 0x48, 0x81, 0xec, 0xd8, 0x00, 0x00, 0x00,
                                                    0x80, 0xb9, 0xb4, 0x2e, 0x00, 0x00, 0x00 };
    // flares: adv_game+0x44c40 (push rbx/rbp/r13/r15; sub rsp,128h)
    static const unsigned char flare_sig[] = { 0x40, 0x53, 0x55, 0x41, 0x55, 0x41, 0x57, 0x48, 0x81, 0xec, 0x28, 0x01, 0x00, 0x00 };
    // beta26 +0x660e0 (mov [rsp+10h],rdx; push rbp/rbx/rsi/rdi/r13/r14; lea rbp,[rsp-68h]; sub rsp,168h)
    static const unsigned char flare_sig26[] = { 0x48, 0x89, 0x54, 0x24, 0x10, 0x55, 0x53, 0x56, 0x57, 0x41, 0x55, 0x41, 0x56, 0x48, 0x8d, 0x6c,
                                                 0x24, 0x98, 0x48, 0x81, 0xec, 0x68, 0x01, 0x00, 0x00 };
    const unsigned char *dsig = BV(detonate_sig, detonate_sig, detonate_sig26), *fsig = BV(flare_sig, flare_sig, flare_sig26);
    size_t dsig_n = BV(sizeof detonate_sig, sizeof detonate_sig, sizeof detonate_sig26), fsig_n = BV(sizeof flare_sig, sizeof flare_sig, sizeof flare_sig26);
    char *detonate = build_code("adv_physic.codelib", BV(0x9b520, 0x9b4b0, 0x111a30), dsig, dsig_n);
    char *flare = build_code("adv_game.codelib", BV(0x44c40, 0x44c40, 0x660e0), fsig, fsig_n);
    if (g_explode && detonate) g_detonate = (Detonate)install_detour(detonate, dsig_n, detonate_hook);
    if (flare) g_flare = (Flare)install_detour(flare, fsig_n, flare_hook);
    mp_log("shots: %s%s; explosions %s (vehicles blown up by their crews: %s), flares %s; off-map fire goes by the side it is fired for",
           !g_launch ? "could not be hooked" : g_enabled ? "a unit's shots are its owner's: sent to the other game, not made up locally" : "watched only",
           g_file ? ", every launch is written to mp_shots" : "", g_explode ? "hooked" : "NOT hooked", g_detonate ? "known" : "not known",
           g_flare ? "hooked" : "NOT hooked");
    if (!g_launch) g_enabled = false;
}

// Every frame: what the other game has not acknowledged goes out - new launches at once, older ones again
// after a pause. Nothing is given up: a launch stays in the queue until the other game has it.
void shots_frame(void (*send)(const void *, int))
{
    if (g_out.empty()) return;
    if (!(session_connected() && session_role() != ROLE_NONE)) { g_out.clear(); return; }
    const size_t per = 5, window = 60;
    DWORD now = GetTickCount();
    size_t n = g_out.size() < window ? g_out.size() : window, at = 0;
    while (at < n) {
        // a datagram starts at the first launch that is due and carries the ones after it
        if (g_out[at].sent && now - g_out[at].sent < 150) { at++; continue; }
        unsigned char packet[sizeof(ShotHead) + per * sizeof(ShotRec)];
        size_t k = n - at < per ? n - at : per;
        ShotHead h = { make_head(PKT_SHOT, (int)k), g_epoch };
        memcpy(packet, &h, sizeof h);
        for (size_t i = 0; i < k; i++) {
            memcpy(packet + sizeof h + i * sizeof(ShotRec), &g_out[at + i].rec, sizeof(ShotRec));
            if (g_out[at + i].sent) g_stat_repeats++;
            g_out[at + i].sent = now ? now : 1;
        }
        send(packet, (int)(sizeof h + k * sizeof(ShotRec)));
        g_stat_sent_bytes += sizeof h + k * sizeof(ShotRec);
        at += k;
    }
}

void shots_on_packet(const unsigned char *data, int size, void (*send)(const void *, int))
{
    const Head *head = (const Head *)data;
    if (head->type == PKT_SHOT_ACK) {
        if (size < (int)sizeof(ShotAck)) return;
        const ShotAck *a = (const ShotAck *)data;
        if (a->epoch != g_epoch) return;
        while (!g_out.empty() && g_out.front().rec.seq <= a->seq) g_out.pop_front();
        return;
    }
    if (size < (int)sizeof(ShotHead) || size < (int)(sizeof(ShotHead) + head->count * sizeof(ShotRec))) return;
    const ShotHead *h = (const ShotHead *)data;
    const ShotRec *recs = (const ShotRec *)(h + 1);
    if (h->epoch != g_epoch_in) {               // the other game started counting again
        g_epoch_in = h->epoch;
        g_last_seq_in = 0;
    }
    for (unsigned i = 0; i < head->count; i++) {
        if (recs[i].seq != g_last_seq_in + 1) continue;         // had already, or not its turn yet
        g_last_seq_in = recs[i].seq;
        g_in.push_back(recs[i]);
    }
    ShotAck ack = { make_head(PKT_SHOT_ACK), g_epoch_in, g_last_seq_in };
    send(&ack, sizeof ack);
}

// A paused game takes no simulation steps, but a ceasefire offered or the battle ending in the other game
// must still arrive: those records are handed on from here when no step has come for a while.
static DWORD g_last_step;

void shots_idle(void)
{
    if (g_in.empty() || GetTickCount() - g_last_step < 300) return;
    std::vector<ShotRec> rest;
    for (const ShotRec &s : g_in) {
        if (s.kind == 8) endgame_received(s);
        else if (s.kind == 10) conditions_received(s);
        else if (s.kind == 11 || s.kind == 12 || s.kind == 15) orders_received(s);      // (orders are given in a paused battle too)
        else if (s.kind == 13) stats_received(s, g_om);
        else if (s.kind == 17) cells_received(s);       // (the ground as the host holds it, before its end)
        else rest.push_back(s);
    }
    g_in.swap(rest);
}

// A new connection: both directions start again.
void shots_reset(void)
{
    g_in.clear();
    g_out.clear();
    g_seq = 0;
    g_epoch = (GetTickCount() ^ (GetCurrentProcessId() << 16) ^ (g_epoch * 2654435761u)) | 1;
    trench_resend();
    xlate_resend();
    endgame_resend();
    conditions_resend();
    orders_reset();
    vdamage_reset();
    guns_reset();
    seats_reset();
    cells_reset();
    mapobj_reset();
}

static void carry_out_explosion(const ShotRec &s)
{
    if (!g_explode) return;
    void *d1 = desc_ptr(xlate_desc(s.weapon)), *d2 = desc_ptr(xlate_desc(s.ammo));
    float two[2];
    u32 code;
    ammo_check(d2, two, &code);
    if ((s.weapon != 0xffffffffu && !d1) || (s.ammo != 0xffffffffu && !d2) || memcmp(two, s.ammo_check, 8) || code != s.ammo_code) { g_stat_refused++; return; }
    alignas(16) char rec[X_SIZE];
    memcpy(rec, s.launch, X_SIZE);
    *(void **)(rec + X_DESC1) = d1;
    *(void **)(rec + X_DESC2) = d2;
    log_explosion("received", rec, 0);
    g_injecting = true;
    g_explode(g_shellphys, rec);
    g_injecting = false;
    g_stat_x_in++;
}

static void carry_out_flare(const ShotRec &s, ObjMan *om)
{
    if (!g_flare) return;
    if (!g_divisions) g_divisions = (char *)engine_find(om, "i_divisions", "divisions");
    if (!g_divisions) { g_stat_refused++; return; }
    alignas(16) float pos[4] = { s.pos[0], s.pos[1], s.pos[2], 0 }, dir[4];
    u32 kind, side;
    int arg6;
    alignas(16) unsigned char params[FLARE_PARAMS];
    memcpy(dir, s.launch, 16);
    memcpy(&kind, s.launch + 16, 4);
    memcpy(&side, s.launch + 20, 4);
    memcpy(&arg6, s.launch + 24, 4);
    memcpy(params, s.launch + 32, FLARE_PARAMS);
    if (g_file) { fprintf(g_file, "F %llu received side=%08x kind=%u pos=%.2f,%.2f,%.2f\n", session_battle_ms(), side, kind, pos[0], pos[1], pos[2]); fflush(g_file); }
    g_injecting = true;
    g_flare(g_divisions, pos, kind, dir, side, arg6, s.launch[28] && g_bi == BUILD_BETA26 ? params : 0);
    g_injecting = false;
    g_stat_f_in++;
}

// Tests: every squad of this player that has off-map support calls a strike onto the middle of the other
// player's units, as the engine's own computer commander does it - the target area into the squad record
// (+0x170 and +0x180: the two ends of the strip, x / z, with two fractions in y / w), +0x194 set, +0x190 cleared, then
// divisions slot 31 (squad, flags, kind, rounds, -1). kind is the mission's ammunition: 0 high explosive,
// 1 smoke, 2 illumination (flares). beta26 (9920-byte squad records): the strip +0x1c0 / +0x1d0, +0x1e4, +0x1e0,
// support +0x318, "has support" +0x448, and the fire mission is slot 32 (adv_game+0xb79b0; a slot was added at 29).
void shots_test_artillery(int rounds, int kind) { g_test_rounds = rounds > 0 ? rounds : 8; g_test_kind = kind; }

static void test_artillery(ObjMan *om)
{
    int rounds = g_test_rounds, kind = g_test_kind, called = 0, able = 0;
    g_test_rounds = 0;
    if (!g_divisions) g_divisions = (char *)engine_find(om, "i_divisions", "divisions");
    float centre[3];
    if (!g_divisions || !session_other_units_centre(centre)) { mp_log("test: artillery - no divisions manager or no other units"); return; }
    void **vt = *(void ***)g_divisions;
    for (u32 squad : session_own_squads()) {
        char *sq = ((char *(*)(void *, u32))vt[11])(g_divisions, squad);
        if (!readable(sq, BF(0x440, 0x4e0))) continue;
        u32 support = *(u32 *)(sq + BF(0x2b0, 0x318));
        if (!sq[BF(0x3e0, 0x448)] || support == 0xffffffffu) continue;
        able++;
        float *a = (float *)(sq + BF(0x170, 0x1c0)), *b = (float *)(sq + BF(0x180, 0x1d0));
        // y and w are not heights: fractions the computer commander draws from 0..0.25 and 0..0.35 (how
        // long the battery takes to work the mission out, how far the rounds scatter)
        a[0] = centre[0] + 25; a[1] = 0.1f; a[2] = centre[2]; a[3] = 0.15f;
        b[0] = centre[0] - 25; b[1] = 0.1f; b[2] = centre[2]; b[3] = 0.15f;
        sq[BF(0x194, 0x1e4)] = 1;
        *(u32 *)(sq + BF(0x190, 0x1e0)) = 0;
        u32 got = ((FireMission)vt[BF(31, 32)])(g_divisions, squad, 0, (u32)kind, (u32)rounds, -1.0f);
        mp_log("test: artillery - squad %08x (support %u, side %08x) called %d rounds of kind %d onto %.0f,%.0f: mission %d",
               squad, support, *(u32 *)(sq + 0x60), rounds, kind, centre[0], centre[2], (int)got);
        if (got != 0xffffffffu) called++;
    }
    mp_log("test: artillery - %d of this player's squads have off-map support, %d called it", able, called);
}

// Tests: the crews of this player's first n vehicles set their charges - the countdown the vehicle's own
// update (adv_physic+0x9b520) runs: +0x1e1c charges set, +0x117c charges left, +0x1184 seconds to go
// (beta26 +0x111a30: +0x2eb4, +0x169c, +0x16a4).
void shots_test_blowup(int n) { g_test_blowup = n > 0 ? n : 1; }

static void test_blowup(ObjMan *om)
{
    int n = g_test_blowup, done = 0;
    g_test_blowup = 0;
    u32 count;
    Object **all = all_objects(om, &count);
    for (u32 i = 0; i < count && done < n; i++) {
        Object *o = all[i];
        if (!readable(o, BF(0x1e60, 0x2ec0)) || strncmp(o->full, "i_techn:", 8) || !session_owns(o)) continue;
        char *v = (char *)o;
        v[BF(0x1e1c, 0x2eb4)] = 1;
        *(u32 *)(v + BF(0x117c, 0x169c)) = 1;
        *(float *)(v + BF(0x1184, 0x16a4)) = 1.0f;
        mp_log("test: blowup - the crew of %s will blow it up in 1 s", o->full);
        done++;
    }
    if (!done) mp_log("test: blowup - this player has no vehicle");
}

// Tests: n flares go up over the other player's units, through the engine's own flare function (divisions
// slot 45, beta26 46) with the side of this player's first squad - as a squad lights them at night (adv_game+0x76180).
void shots_test_flares(int n) { g_test_flares = n > 0 ? n : 1; }

static void test_flares(ObjMan *om)
{
    int n = g_test_flares;
    g_test_flares = 0;
    if (!g_divisions) g_divisions = (char *)engine_find(om, "i_divisions", "divisions");
    float centre[3];
    if (!g_divisions || !session_other_units_centre(centre)) { mp_log("test: flares - no divisions manager or no other units"); return; }
    void **vt = *(void ***)g_divisions;
    for (u32 squad : session_own_squads()) {
        char *sq = ((char *(*)(void *, u32))vt[11])(g_divisions, squad);
        if (!readable(sq, BF(0x440, 0x4e0))) continue;
        u32 side = *(u32 *)(sq + 0x60);
        for (int i = 0; i < n; i++) {
            alignas(16) float pos[4] = { centre[0] + 20.0f * i, centre[1] + 1, centre[2], 0 }, dir[4] = { 4.0f + i, 35, 6.0f - i, 0 };
            ((Flare)vt[BF(45, 46)])(g_divisions, pos, 4, dir, side, 0, 0);
        }
        mp_log("test: flares - %d flares for squad %08x (side %08x) over %.0f,%.0f", n, squad, side, centre[0], centre[2]);
        return;
    }
    mp_log("test: flares - this player has no squad");
}

// Before a simulation step: the other game's launches are carried out here.
void shots_sim_step(ObjMan *om)
{
    fires_sim_step(om);
    g_last_step = GetTickCount();
    if (g_test_rounds) test_artillery(om);
    if (g_test_blowup) test_blowup(om);
    if (g_test_flares) test_flares(om);
    if (g_in.empty()) return;
    for (const ShotRec &s : g_in) {
        if (s.kind == 2) trench_received(s);
        else if (s.kind == 3 || s.kind == 4) xlate_received(s, om);
        else if (s.kind == 5) crater_received(s);
        else if (s.kind == 6) stamp_received(s);
        else if (s.kind == 7) hits_received(s);
        else if (s.kind == 8) endgame_received(s);
        else if (s.kind == 10) conditions_received(s);
        else if (s.kind == 11 || s.kind == 12 || s.kind == 15) orders_received(s);
        else if (s.kind == 13) stats_received(s, om);
        else if (s.kind == 14) fires_received(s, om);
        else if (s.kind == 16) seats_received(s);
        else if (s.kind == 17) cells_received(s);
        else if (s.kind == 18) mapobj_received(s);
        else if (s.kind == 19) vdamage_track_received(s);
    }
    if (!g_launch || !g_enabled) { g_in.clear(); return; }
    if (!g_shellphys) g_shellphys = (char *)engine_find(om, "i_shellphys", "shellphys");
    if (!g_shellphys) { g_in.clear(); return; }
    g_om = om;
    for (const ShotRec &s : g_in) {
        if (s.kind == SHOT_EXPLOSION) { carry_out_explosion(s); continue; }
        if (s.kind == SHOT_FLARE) { carry_out_flare(s, om); continue; }
        if (s.kind != SHOT_LAUNCH) continue;
        void *ammo = desc_ptr(xlate_desc(s.ammo)), *weapon = desc_ptr(xlate_desc(s.weapon));
        float two[2];
        u32 code;
        ammo_check(ammo, two, &code);
        Object *shooter = s.shooter ? session_unit(s.shooter) : 0;
        // not the same ammunition here, or a unit this game does not have: no shot rather than a wrong one
        if (!ammo || !weapon || memcmp(two, s.ammo_check, 8) || code != s.ammo_code || (s.shooter && !shooter)) { g_stat_refused++; continue; }
        char rec[L_SIZE];
        memcpy(rec, s.launch, L_SIZE);
        *(void **)(rec + L_AMMO) = ammo;
        *(void **)(rec + L_WEAPON) = weapon;
        *(void **)(rec + L_SHOOTER) = shooter;
        *(void **)(rec + L_OBJ2) = s.obj2 ? session_unit(s.obj2) : 0;
        *(void **)(rec + L_DESC3) = desc_ptr(xlate_desc(s.desc3));
        float pos[4];
        memcpy(pos, s.pos, 16);
        log_launch("received", 'L', rec, s.flags, 0, 0);
        g_injecting = true;
        g_launch(g_shellphys, rec, s.flags & ~2u, s.has_pos ? pos : 0, s.obj5 ? session_unit(s.obj5) : 0, s.arg6);
        g_injecting = false;
        g_stat_injected++;
    }
    g_in.clear();
}

void shots_report(void)
{
    if (g_stat_own || g_stat_dropped || g_stat_injected || g_stat_refused)
        mp_log("shots: 5 s: %llu fired by this game's units and sent (%.1f KB/s, %llu repeats, %zu waiting for an answer), %llu received and fired here, %llu local ones for the other player's units dropped, %llu refused (ammunition or unit not matched)",
               g_stat_own, g_stat_sent_bytes / 5120.0, g_stat_repeats, g_out.size(), g_stat_injected, g_stat_dropped, g_stat_refused);
    g_stat_own = g_stat_dropped = g_stat_injected = g_stat_refused = g_stat_sent_bytes = g_stat_repeats = 0;
    if (g_stat_offmap || g_stat_x_own || g_stat_x_dropped || g_stat_x_in || g_stat_f_own || g_stat_f_dropped || g_stat_f_in)
        mp_log("shots: 5 s: %llu of this game's launches were off-map fire; explosions: %llu made here and sent, %llu received, %llu local ones dropped; flares: %llu fired here and sent, %llu received, %llu local ones dropped",
               g_stat_offmap, g_stat_x_own, g_stat_x_in, g_stat_x_dropped, g_stat_f_own, g_stat_f_in, g_stat_f_dropped);
    g_stat_offmap = g_stat_x_own = g_stat_x_dropped = g_stat_x_in = g_stat_f_own = g_stat_f_dropped = g_stat_f_in = 0;
}
