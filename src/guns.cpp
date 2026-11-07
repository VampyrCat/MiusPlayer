// Where a vehicle's guns point (mpmod2). The angles came only with a vehicle's saved state, a few times a
// minute; in between the other game's own gunner turned the turret wherever IT would aim, and the next state
// put it back - a turret seen turning away and snapping back. They now travel with the transforms:
//
// A vehicle's weapons: an array at vehicle +0x1600 (pointer; count at +0x160c), 0x1d0 bytes each; the two angles
// (radians: traverse, elevation) at +0x12c and +0x130. public_beta-26494568 (IDA): the array is at +0x1b48, count
// +0x1b54 (i_techn slots 92, 93 and 317, adv_physic+0xdd4e0 / +0xdd500 / +0xde130), the records the same - its
// slot 193 (+0x12c7b0, slot 191 +0xada20 before) writes each head's weapons +0x12c..+0x13c as it always did.
// The game that owns a vehicle sends its guns' angles with the transforms (15 times a second, a vehicle when they
// have moved, and every second); the other game turns its copy's guns there before every simulation step,
// moving smoothly from the angles before to the newest over the time between two packets.
//   PKT_GUNS: Head (count), then per vehicle: u32 id, u8 weapons, u8 pad[3], f32 angles[weapons][2]
// MP_SYNC_GUNS=0 switches it off.
#include "common.h"
#include "build.h"
#include "protocol.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <unordered_map>
#include <vector>

namespace {
enum { WPN_SIZE = 0x1d0, WPN_ANGLES = 0x12c, MAX_GUNS = 6, ROOM = 1150 };
struct GunAim { float prev[MAX_GUNS][2], cur[MAX_GUNS][2]; unsigned n; double at, interval; };
struct GunSent { float a[MAX_GUNS][2]; unsigned n; DWORD when; };
}
static std::unordered_map<u32, GunAim> g_aim;          // the owner's angles per vehicle (not owned here)
static std::unordered_map<u32, GunSent> g_gun_sent;     // what this game sent last per vehicle
static bool g_enabled = true, g_checked;
static u64 g_stat_sent, g_stat_bytes, g_stat_set;

static double seconds_now(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}

static bool on(void)
{
    if (!g_checked) { g_checked = true; char v[8]; if (GetEnvironmentVariableA("MP_SYNC_GUNS", v, sizeof v)) g_enabled = atoi(v) != 0; }
    return g_enabled && session_host_auth() && session_connected() && session_simulating() && session_versus_or_coop();
}

static bool weapons(Object *o, char **arr, unsigned *n)
{
    char *v = (char *)o;
    size_t list = BF(0x1600, 0x1b48);           // the array; its count 12 bytes on
    if (!readable(v + list, 16)) return false;
    *arr = *(char **)(v + list);
    *n = *(u32 *)(v + list + 0xc);
    if (!*arr || !*n || *n > 16 || !readable(*arr, (size_t)*n * WPN_SIZE)) return false;
    if (*n > MAX_GUNS) *n = MAX_GUNS;
    return true;
}

void guns_frame(ObjMan *om, void (*send)(const void *, int))
{
    if (!on()) return;
    DWORD now = GetTickCount();
    unsigned char buf[sizeof(Head) + ROOM];
    size_t used = sizeof(Head);
    unsigned count = 0;
    auto flush = [&]() {
        if (!count) return;
        Head h = make_head(PKT_GUNS, count);
        memcpy(buf, &h, sizeof h);
        send(buf, (int)used);
        g_stat_bytes += used;
        used = sizeof(Head);
        count = 0;
    };
    u32 n_all;
    Object **all = all_objects(om, &n_all);
    for (u32 i = 0; i < n_all; i++) {
        Object *o = all[i];
        u64 key;
        char *arr;
        unsigned n;
        if (strcmp(o->iface, "i_techn") || !session_unit_key(o, &key) || !session_owns(o) || !weapons(o, &arr, &n)) continue;
        u32 id = (u32)key;
        GunSent now_a = {};
        now_a.n = n;
        for (unsigned k = 0; k < n; k++) memcpy(now_a.a[k], arr + (size_t)k * WPN_SIZE + WPN_ANGLES, 8);
        auto it = g_gun_sent.find(id);
        bool moved = it == g_gun_sent.end() || it->second.n != n;
        for (unsigned k = 0; !moved && k < n; k++)
            moved = fabsf(it->second.a[k][0] - now_a.a[k][0]) > 0.0005f || fabsf(it->second.a[k][1] - now_a.a[k][1]) > 0.0005f;
        if (!moved && now - it->second.when < 1000) continue;
        size_t size = 8 + (size_t)n * 8;
        if (used + size > sizeof buf) flush();
        unsigned char *p = buf + used;
        memcpy(p, &id, 4);
        p[4] = (unsigned char)n;
        p[5] = p[6] = p[7] = 0;
        memcpy(p + 8, now_a.a, (size_t)n * 8);
        used += size;
        count++;
        now_a.when = now;
        g_gun_sent[id] = now_a;
        g_stat_sent++;
    }
    flush();
}

void guns_on_packet(const unsigned char *data, int size)
{
    if (size < (int)sizeof(Head)) return;
    Head h;
    memcpy(&h, data, sizeof h);
    size_t at = sizeof(Head);
    double now = seconds_now();
    for (unsigned i = 0; i < h.count && at + 8 <= (size_t)size; i++) {
        u32 id;
        memcpy(&id, data + at, 4);
        unsigned n = data[at + 4];
        if (n > MAX_GUNS || at + 8 + n * 8 > (size_t)size) return;
        GunAim &g = g_aim[id];
        bool first = g.n == 0 || g.n != n;
        if (!first) {
            double gap = now - g.at;
            if (gap > 0.005 && gap < 1.5) g.interval = g.interval > 0 ? g.interval + 0.2 * (gap - g.interval) : gap;
            memcpy(g.prev, g.cur, sizeof g.prev);
        }
        memcpy(g.cur, data + at + 8, (size_t)n * 8);
        if (first) memcpy(g.prev, g.cur, sizeof g.prev);
        g.n = n;
        g.at = now;
        at += 8 + (size_t)n * 8;
    }
}

// the way from a to b the short way round (angles in radians)
static float toward(float a, float b, float k)
{
    float d = b - a;
    while (d > 3.14159265f) d -= 6.2831853f;
    while (d < -3.14159265f) d += 6.2831853f;
    return a + d * k;
}

void guns_sim_step(ObjMan *)
{
    if (!on() || g_aim.empty()) return;
    double now = seconds_now();
    for (auto &kv : g_aim) {
        Object *o = session_unit((1ull << 32) | kv.first);
        char *arr;
        unsigned n;
        if (!o || session_owns(o) || !weapons(o, &arr, &n)) continue;
        const GunAim &g = kv.second;
        double span = g.interval > 0.03 ? g.interval : 0.066;
        float k = (float)((now - g.at) / span);
        k = k < 0 ? 0 : k > 1 ? 1 : k;
        for (unsigned w = 0; w < n && w < g.n; w++) {
            float *a = (float *)(arr + (size_t)w * WPN_SIZE + WPN_ANGLES);
            for (int j = 0; j < 2; j++) {
                float want = toward(g.prev[w][j], g.cur[w][j], k);
                if (a[j] != want) { a[j] = want; g_stat_set++; }
            }
        }
    }
}

void guns_reset(void) { g_aim.clear(); g_gun_sent.clear(); }

void guns_report(void)
{
    if (g_stat_sent || g_stat_set)
        mp_log("guns: 5 s: %llu vehicles' gun angles sent (%.1f KB/s), %llu angles put into the other player's vehicles here",
               g_stat_sent, g_stat_bytes / 5120.0, g_stat_set);
    g_stat_sent = g_stat_bytes = g_stat_set = 0;
}
