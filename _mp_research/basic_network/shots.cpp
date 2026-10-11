// Shots: every shell, bullet, grenade and flare in a battle is created by one engine function, the
// "launch" of the shell manager (i_shellphys, virtual slot 3, basic_physic+0x18430). It makes the
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
// MP_SYNC_SHOTS=0 switches all of this off; MP_LOG_SHOTS=1 writes every launch to out\mp_shots_<pid>.txt.
#include "common.h"
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
static Launch g_launch;
static Explode g_explode;
static ObjMan *g_om;
static char *g_shellphys;
static DWORD g_main_thread;
static bool g_injecting, g_enabled = true;
static FILE *g_file;
static u32 g_seq, g_epoch, g_last_seq_in, g_epoch_in, g_dumped;
static u64 g_stat_own, g_stat_dropped, g_stat_injected, g_stat_refused, g_stat_free, g_stat_sent_bytes, g_stat_repeats;
struct Pending { ShotRec rec; DWORD sent; };
static std::deque<Pending> g_out;               // launched here, not yet acknowledged by the other game
static std::vector<ShotRec> g_in;               // received, carried out before the next simulation step

// launch record: pointers at +0x38 (ammunition), +0x40 (weapon), +0x48 (firing object), +0x50 (object),
// +0x60 (description)
enum { L_AMMO = 0x38, L_WEAPON = 0x40, L_SHOOTER = 0x48, L_OBJ2 = 0x50, L_DESC3 = 0x60, L_SIZE = 0x90 };

static u32 desc_id(void *p) { return p ? ((u32 (*)(ObjMan *, void *))g_om->vtbl[56])(g_om, p) : 0xffffffffu; }
static void *desc_ptr(u32 id) { return id == 0xffffffffu ? 0 : ((void *(*)(ObjMan *, u32))g_om->vtbl[57])(g_om, id); }

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

// Whose launch is it? A unit's launch belongs to the game that owns the unit; anything else (a launch with
// no firing unit behind it) to the host.
static bool mine(void *shooter)
{
    if (shooter && is_object(shooter)) return session_owns((Object *)shooter);
    return session_role() == ROLE_HOST;
}

static void queue_shot(char kind, const char *rec, u64 flags, const float *pos, void *obj5, u32 arg6)
{
    ShotRec s = {};
    if (!g_epoch) g_epoch = (GetTickCount() ^ (GetCurrentProcessId() << 16)) | 1;
    s.seq = ++g_seq;
    s.kind = kind == 'L' ? 0 : 1;
    s.flags = (unsigned char)flags;
    s.has_pos = pos != 0;
    void *ammo = *(void **)(rec + L_AMMO);
    s.ammo = desc_id(ammo);
    s.weapon = desc_id(*(void **)(rec + L_WEAPON));
    ammo_check(ammo, s.ammo_check, &s.ammo_code);
    if (kind == 'L') {
        s.desc3 = desc_id(*(void **)(rec + L_DESC3));
        s.shooter = object_key(*(void **)(rec + L_SHOOTER));
        s.obj2 = object_key(*(void **)(rec + L_OBJ2));
        s.obj5 = object_key(obj5);
        s.arg6 = arg6;
        if (pos) memcpy(s.pos, pos, 16);
        memcpy(s.launch, rec, L_SIZE);
        memset(s.launch + L_AMMO, 0, 0x20);         // the four pointers
        memset(s.launch + L_DESC3, 0, 8);
    } else {
        s.desc3 = 0xffffffffu;
        memcpy(s.launch, rec, 0x40);                // an explosion is a place and the two descriptions
    }
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
    bool repl = replicating(), own = !repl || mine(shooter);
    log_launch(!repl ? "free" : own ? "own" : "dropped", 'L', rec, flags, obj5, _ReturnAddress());
    if (!repl) { g_stat_free++; g_launch(sp, rec, flags, pos, obj5, arg6); return; }
    if (!own) { g_stat_dropped++; return; }
    g_stat_own++;
    g_launch(sp, rec, flags, pos, obj5, arg6);
    if (*(void **)(rec + L_AMMO) && *(void **)(rec + L_WEAPON)) queue_shot('L', rec, flags, pos, obj5, arg6);
}

// Virtual slot 4 (basic_physic+0x19260): an ammunition going off at a place, without a flight - only
// watched for now (MP_LOG_SHOTS), to learn what uses it.
static void explode_hook(char *sp, char *rec)
{
    if (g_file && GetCurrentThreadId() == g_main_thread && readable(rec, 0x50)) {
        char fake[L_SIZE] = {};
        memcpy(fake, rec, 0x30);
        memcpy(fake + L_AMMO, rec + 0x48, 8);
        memcpy(fake + L_WEAPON, rec + 0x40, 8);
        log_launch("explosion", 'X', fake, 0, 0, _ReturnAddress());
    }
    g_explode(sp, rec);
}

void shots_install(ObjMan *om)
{
    static const unsigned char launch_sig[] = { 0x48, 0x89, 0x54, 0x24, 0x10, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56,
                                                0x48, 0x8d, 0xac, 0x24, 0xe8, 0xfd, 0xff, 0xff };
    static const unsigned char explode_sig[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x81, 0xec, 0x70, 0x02, 0x00, 0x00, 0x48, 0x8b, 0xf9 };
    static bool done;
    if (done) return;
    HMODULE m = GetModuleHandleA("basic_physic.codelib");
    if (!m) return;
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
    char *launch = (char *)m + 0x18430, *explode = (char *)m + 0x19260;
    if (memcmp(launch, launch_sig, sizeof launch_sig)) { mp_log("shots: unknown game build, each game fires for itself"); g_enabled = false; return; }
    g_launch = (Launch)install_detour(launch, sizeof launch_sig, launch_hook);
    if (g_file && !memcmp(explode, explode_sig, sizeof explode_sig)) g_explode = (Explode)install_detour(explode, sizeof explode_sig, explode_hook);
    mp_log("shots: %s%s", !g_launch ? "could not be hooked" : g_enabled ? "a unit's shots are its owner's: sent to the other game, not made up locally" : "watched only",
           g_file ? ", every launch is written to mp_shots" : "");
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

// A new connection: both directions start again.
void shots_reset(void)
{
    g_in.clear();
    g_out.clear();
    g_seq = 0;
    g_epoch = (GetTickCount() ^ (GetCurrentProcessId() << 16) ^ (g_epoch * 2654435761u)) | 1;
    trench_resend();
    xlate_resend();
}

// Before a simulation step: the other game's launches are carried out here.
void shots_sim_step(ObjMan *om)
{
    if (g_in.empty()) return;
    for (const ShotRec &s : g_in) {
        if (s.kind == 2) trench_received(s);
        else if (s.kind == 3 || s.kind == 4) xlate_received(s, om);
    }
    if (!g_launch || !g_enabled) { g_in.clear(); return; }
    if (!g_shellphys) g_shellphys = (char *)engine_find(om, "i_shellphys", "shellphys");
    if (!g_shellphys) { g_in.clear(); return; }
    g_om = om;
    for (const ShotRec &s : g_in) {
        if (s.kind != 0) continue;
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
    if (!g_stat_own && !g_stat_dropped && !g_stat_injected && !g_stat_refused) return;
    mp_log("shots: 5 s: %llu fired by this game's units and sent (%.1f KB/s, %llu repeats, %zu waiting for an answer), %llu received and fired here, %llu local ones for the other player's units dropped, %llu refused (ammunition or unit not matched)",
           g_stat_own, g_stat_sent_bytes / 5120.0, g_stat_repeats, g_out.size(), g_stat_injected, g_stat_dropped, g_stat_refused);
    g_stat_own = g_stat_dropped = g_stat_injected = g_stat_refused = g_stat_sent_bytes = g_stat_repeats = 0;
}
