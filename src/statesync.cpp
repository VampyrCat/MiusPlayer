// Replication of full object state (see state.cpp) from the game that owns an object to the other game.
//
// The owner saves each object a few times a second. When the bytes differ from what the other side is
// known to have, it sends the difference: XOR against that known version, run-length encoded, cut into
// datagrams. The receiver rebuilds the blob, acknowledges the version, and loads it into its own copy of
// the object before the next simulation step. Versions only advance on acknowledgement, so a lost
// datagram costs one retry and never corrupts state.
#include "common.h"
#include "protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unordered_map>
#include <vector>

typedef std::vector<unsigned char> Blob;

// ---------------------------------------------------------------- delta coding
static void put_varint(Blob &out, u32 v)
{
    while (v >= 0x80) { out.push_back((unsigned char)(v | 0x80)); v >>= 7; }
    out.push_back((unsigned char)v);
}

static bool get_varint(const unsigned char *&p, const unsigned char *end, u32 &v)
{
    v = 0;
    for (int shift = 0; p < end && shift < 35; shift += 7) {
        unsigned char b = *p++;
        v |= (u32)(b & 0x7f) << shift;
        if (!(b & 0x80)) return true;
    }
    return false;
}

// (unchanged run, changed run, changed bytes)* ; bytes are cur XOR base, base padded with zeros
static void encode_delta(const Blob &base, const Blob &cur, Blob &out)
{
    out.clear();
    size_t n = cur.size(), i = 0;
    auto same = [&](size_t k) { return cur[k] == (k < base.size() ? base[k] : 0); };
    while (i < n) {
        size_t z = i;
        while (z < n && same(z)) z++;
        size_t l = z;
        // a changed run ends at four or more unchanged bytes (shorter gaps are cheaper to send as data)
        for (size_t gap = 0; l < n; l++) {
            if (same(l)) { if (++gap >= 4) { l -= gap - 1; break; } }
            else gap = 0;
        }
        if (l > n) l = n;
        while (l > z && same(l - 1)) l--;
        put_varint(out, (u32)(z - i));
        put_varint(out, (u32)(l - z));
        for (size_t k = z; k < l; k++) out.push_back(cur[k] ^ (k < base.size() ? base[k] : 0));
        i = l;
    }
}

static bool decode_delta(const Blob &base, const unsigned char *p, size_t len, u32 raw_size, Blob &out)
{
    if (raw_size > (16u << 20)) return false;
    out.assign(raw_size, 0);
    memcpy(out.data(), base.data(), min(base.size(), (size_t)raw_size));
    const unsigned char *end = p + len;
    size_t i = 0;
    while (p < end) {
        u32 z, l;
        if (!get_varint(p, end, z) || !get_varint(p, end, l)) return false;
        i += z;
        if (i + l > raw_size || p + l > end) return false;
        for (u32 k = 0; k < l; k++) {
            // XOR against the padded base: bytes beyond the old size were zero
            out[i + k] = (unsigned char)((i + k < base.size() ? base[i + k] : 0) ^ p[k]);
        }
        p += l;
        i += l;
    }
    return true;
}

// ---------------------------------------------------------------- shared
static const u32 k_frag = 1100;
static u64 g_stat_bytes, g_stat_objects, g_stat_raw, g_stat_loads, g_stat_bad, g_stat_received, g_stat_unsafe;
static u64 g_stat_rejected;
static bool g_translate;      // versus: the two games see every unit from opposite sides

u64 statesync_key(Object *o)
{
    u64 kind = !strcmp(o->iface, "i_techn") ? 1 : !strcmp(o->iface, "i_human") ? 2 : 3;
    if (kind == 3) return ((u64)3 << 32) | fnv((const unsigned char *)o->full, strlen(o->full));
    const char *us = strrchr(o->full, '_');
    return (kind << 32) | (us ? strtoul(us + 1, 0, 16) : 0);
}

// Singleton simulation objects that are replicated besides the units. Each was checked to load cleanly
// into a running battle; the others either hold only local presentation state or are too large to matter.
// (Checked on the 2024 builds only: on public_beta-26494568 MP_SYNC_WORLD stays off - those classes were
// rebuilt and grew there, and what their states hold has not been looked at.)
static bool replicated_singleton(Object *o)
{
    static const char *names[] = { "i_divisions", "i_statistic", "i_trigman", "i_formation", "i_fireman", "i_shellphys" };
    static int enabled = -1;
    if (enabled < 0) {
        char v[8];
        enabled = GetEnvironmentVariableA("MP_SYNC_WORLD", v, sizeof v) ? atoi(v) : 0;
        if (enabled && !BF(1, NP)) {
            mp_log("state: MP_SYNC_WORLD is not ported to this build, only units are replicated");
            enabled = 0;
        }
    }
    if (!enabled) return false;
    for (int i = 0; i < enabled && i < 6; i++)
        if (!strcmp(o->iface, names[i])) return true;
    return false;
}

static bool replicated(Object *o) { return state_is_unit(o) || replicated_singleton(o); }

// ---------------------------------------------------------------- owner side
struct StateSent { u32 ver; Blob blob; DWORD at; bool again; };     // at: when it first went out; again: sent more than once
struct Track {
    Blob base;                     // the newest version the other side has confirmed
    u32 base_ver = 0, last_ver = 0;
    std::vector<StateSent> sent_versions;   // sent since then, newest last; any of them may be confirmed next
    DWORD checked = 0, sent = 0;
};

// How fast states are sent (KB a second). Up to MP_STATE_KBPS (600), and only as fast as the line carries:
// states are most of what the host sends (a skirmish fills the 600 KB/s, ~4.8 Mbit/s), and sent faster than a
// player's upload carries they fill his router's queue - every transform, shot and order behind them waits up
// to its length and then is lost with them. So the rate follows the round trip of the states' own
// acknowledgements: queued time (the round trip less the smallest of the last 30 s) above 150 ms, or more
// than a third of the versions not acknowledged while acknowledgements do come, takes 30 % off (at most every
// half second); below 80 ms it grows by 10 % (+6 KB/s) four times a second - 150 to 600 KB/s in about four
// seconds. (Each game looks at what arrived once a frame, so the round trip wanders by a frame or two of each
// - 30 to 60 ms - with nothing queued; a game still loading the battle answers nothing at all, which is not a
// line losing everything.) MP_STATE_ADAPT=0: the fixed rate as before.
static int g_rate_max = -1, g_adapt;
static void rate_init(void)
{
    if (g_rate_max >= 0) return;
    char v[16];
    g_rate_max = GetEnvironmentVariableA("MP_STATE_KBPS", v, sizeof v) ? max(atoi(v), 1) : 600;
    g_adapt = GetEnvironmentVariableA("MP_STATE_ADAPT", v, sizeof v) ? atoi(v) : 1;
}
static double start_rate(void) { rate_init(); return g_adapt ? min(150.0, (double)g_rate_max) : g_rate_max; }

// Everything about one game the states go to: the other player (0) and, on a host with more players, each
// extra player (1-3, session.cpp) - its versions, its rate and its round trip.
struct Owner {
    bool on = false;
    std::unordered_map<u64, Track> tracks;
    u32 cursor = 0, epoch = 0;     // epoch: this game's versions for it (see ObjectFrag)
    double budget = 0, rate = 0;
    DWORD last_frame = 0, ctl_at = 0, cut_at = 0;
    u64 ctl_versions = 0, ctl_acked = 0, versions_total = 0, seen = 0;
    float srtt = 0, slot_min[6] = { -1, -1, -1, -1, -1, -1 };      // round trip: smoothed, the least of each 5 s
    int slot = 0;
    DWORD slot_at = 0;
    double rate_sum = 0;
    u32 rate_n = 0;
    u64 st_versions = 0, st_acked = 0, st_restarts = 0, st_cuts = 0, st_objects = 0, st_bytes = 0;
    void (*send)(const void *, int) = 0;               // the other player
    void (*send_ch)(int, const void *, int) = 0;       // an extra player, by channel
};
enum { OWNERS = 4 };
static Owner g_own[OWNERS];

static void owner_start(Owner &o, int ch)
{
    o.tracks.clear();
    o.epoch = (GetTickCount() ^ (GetCurrentProcessId() << 16) ^ (o.epoch * 2654435761u) ^ (u32)ch) | 1;
    o.rate = start_rate();
    o.ctl_versions = o.ctl_acked = 0;
    o.srtt = 0;
    for (float &v : o.slot_min) v = -1;
    o.slot_at = 0;
}

static void rtt_sample(Owner &o, u32 ms)
{
    if (&o == &g_own[0]) net_rtt_sample(ms);       // (the session's estimate is the other player's)
    if (ms > 20000) return;
    float m = (float)ms;
    o.srtt = o.srtt <= 0 ? (m > 0 ? m : 0.5f) : o.srtt + 0.125f * (m - o.srtt);
    DWORD now = GetTickCount();
    if (!o.slot_at || now - o.slot_at >= 5000) { o.slot = (o.slot + 1) % 6; o.slot_min[o.slot] = m; o.slot_at = now ? now : 1; }
    else if (m < o.slot_min[o.slot]) o.slot_min[o.slot] = m;
}
static float rtt_least(const Owner &o)
{
    float best = -1;
    for (float v : o.slot_min) if (v >= 0 && (best < 0 || v < best)) best = v;
    return best < 0 ? 0 : best;
}

static void rate_control(Owner &o, DWORD now)
{
    if (!g_adapt || now - o.ctl_at < 250) return;
    o.ctl_at = now;
    if (o.versions_total == o.seen) return;       // (nothing sent: a paused battle - the round trip is of before)
    o.seen = o.versions_total;
    double queued = o.srtt > 0 ? o.srtt - rtt_least(o) : 0;
    // (versions sent and acknowledged over the last second or so: the acknowledgements trail by a round trip)
    double lost = o.ctl_versions >= 12 && o.ctl_acked ? 1.0 - (double)o.ctl_acked / o.ctl_versions : 0;
    if (o.ctl_versions >= 48) { o.ctl_versions /= 2; o.ctl_acked /= 2; }
    if ((queued > 150 || lost > 0.33) && now - o.cut_at >= 500) {
        o.rate = max(o.rate * 0.7, 20.0);
        o.cut_at = now;
        o.st_cuts++;
    } else if (queued < 80 && lost < 0.2 && now - o.cut_at >= 1000)
        o.rate = min(o.rate * 1.10 + 6, (double)g_rate_max);
}

static void send_object(Owner &o, int ch, u64 key, Track &t, DWORD now, bool again)
{
    if (again) t.sent_versions.back().again = true;
    else { o.st_versions++; o.ctl_versions++; o.versions_total++; }
    static Blob delta;
    const Blob &cur = t.sent_versions.back().blob;
    encode_delta(t.base, cur, delta);
    u32 nfrags = (u32)((delta.size() + k_frag - 1) / k_frag);
    if (!nfrags) nfrags = 1;
    for (u32 f = 0; f < nfrags; f++) {
        unsigned char packet[sizeof(ObjectFrag) + k_frag];
        ObjectFrag *h = (ObjectFrag *)packet;
        u32 off = f * k_frag, len = (u32)min((size_t)k_frag, delta.size() - off);
        h->h = make_head(PKT_OBJECT, (int)len);
        h->key = key;
        h->version = t.sent_versions.back().ver;
        h->base_version = t.base_ver;
        h->raw_size = (u32)cur.size();
        h->delta_size = (u32)delta.size();
        h->frag = (unsigned short)f;
        h->nfrags = (unsigned short)nfrags;
        h->epoch = o.epoch;
        memcpy(h + 1, delta.data() + off, len);
        if (o.send) o.send(packet, (int)(sizeof(ObjectFrag) + len));
        else if (o.send_ch) o.send_ch(ch, packet, (int)(sizeof(ObjectFrag) + len));
        o.budget -= sizeof(ObjectFrag) + len + 28;
        o.st_bytes += sizeof(ObjectFrag) + len;
        if (!ch) g_stat_bytes += sizeof(ObjectFrag) + len;
    }
    t.sent = now;
    o.st_objects++;
    if (!ch) { g_stat_objects++; g_stat_raw += cur.size(); }
}

static void owner_frame(Owner &o, int ch, ObjMan *om, DWORD now, bool (*owns)(Object *))
{
    static Blob blob;
    rate_init();
    if (!o.epoch) owner_start(o, ch);
    rate_control(o, now);
    double dt = o.last_frame ? (now - o.last_frame) / 1000.0 : 0;
    o.last_frame = now;
    o.budget += dt * o.rate * 1024;
    if (o.budget > o.rate * 1024 * 0.25) o.budget = o.rate * 1024 * 0.25;       // at most a quarter second saved up
    o.rate_sum += o.rate;
    o.rate_n++;
    u32 count;
    Object **all = all_objects(om, &count);
    for (u32 n = 0; n < count && o.budget > 0; n++) {
        Object *obj = all[o.cursor++ % count];
        if (!replicated(obj) || !owns(obj) || !session_state_wanted(obj)) continue;
        u64 key = statesync_key(obj);
        Track &t = o.tracks[key];
        bool unit = state_is_unit(obj);
        if (now - t.checked < (!strcmp(obj->iface, "i_techn") ? 700u : 400u)) continue;
        t.checked = now;
        if (!state_save(obj, blob)) continue;
        if (unit && blob.size() >= UNIT_MOTION_SIZE) memset(blob.data(), 0, UNIT_MOTION_SIZE);
        const Blob &newest = t.sent_versions.empty() ? t.base : t.sent_versions.back().blob;
        if (blob.size() == newest.size() && !memcmp(blob.data(), newest.data(), blob.size())) {
            if (!t.sent_versions.empty() && now - t.sent > 400) send_object(o, ch, key, t, now, true);    // not confirmed: again
            continue;
        }
        if (t.sent_versions.size() >= 6) t.sent_versions.erase(t.sent_versions.begin());
        t.sent_versions.push_back({ ++t.last_ver, blob, now, false });
        send_object(o, ch, key, t, now, false);
    }
}

// Called every frame by the side that owns objects. `owns` decides per object.
void statesync_owner_frame(ObjMan *om, DWORD now, bool (*owns)(Object *), void (*send)(const void *, int))
{
    Owner &o = g_own[0];
    o.on = true;
    o.send = send;
    owner_frame(o, 0, om, now, owns);
}

// Host: the same for an extra player (channel 1-3); its states are sent by send(ch, ...).
void statesync_owner_frame_extra(int ch, ObjMan *om, DWORD now, bool (*owns)(Object *), void (*send)(int, const void *, int))
{
    if (ch <= 0 || ch >= OWNERS || !g_own[ch].on) return;
    Owner &o = g_own[ch];
    o.send = 0;
    o.send_ch = send;
    owner_frame(o, ch, om, now, owns);
}

void statesync_reset(void)
{
    owner_start(g_own[0], 0);
}

void statesync_open_extra(int ch)
{
    if (ch <= 0 || ch >= OWNERS) return;
    owner_start(g_own[ch], ch);
    g_own[ch].on = true;
    g_own[ch].last_frame = 0;
    g_own[ch].budget = 0;
}

void statesync_close_extra(int ch)
{
    if (ch <= 0 || ch >= OWNERS) return;
    g_own[ch].on = false;
    g_own[ch].tracks.clear();
}

// An acknowledgement: the other side reports the version it now holds. Later sends are differences against that one.
static void acknowledged(Owner &o, const ObjectAck *a)
{
    if (a->epoch != o.epoch) return;
    auto it = o.tracks.find(a->key);
    if (it == o.tracks.end() || a->version == it->second.base_ver) return;
    Track &t = it->second;
    // (an older one: an answer overtaken by a later one on the way - the other side never goes back)
    if (a->version && a->version < t.base_ver) return;
    for (size_t i = 0; i < t.sent_versions.size(); i++)
        if (t.sent_versions[i].ver == a->version) {
            if (!t.sent_versions[i].again) rtt_sample(o, GetTickCount() - t.sent_versions[i].at);
            o.st_acked++;
            o.ctl_acked++;
            t.base.swap(t.sent_versions[i].blob);
            t.base_ver = a->version;
            t.sent_versions.erase(t.sent_versions.begin(), t.sent_versions.begin() + i + 1);
            return;
        }
    // it holds something we no longer keep: start over from nothing
    o.st_restarts++;
    t.base.clear();
    t.base_ver = 0;
}

// Host: what an extra player's game sends about states - only its acknowledgements (it owns nothing).
void statesync_on_packet_extra(int ch, const unsigned char *data, int size)
{
    if (ch <= 0 || ch >= OWNERS || !g_own[ch].on) return;
    const Head *head = (const Head *)data;
    if (head->type == PKT_OBJECT_ACK && size >= (int)sizeof(ObjectAck)) acknowledged(g_own[ch], (const ObjectAck *)data);
}

// ---------------------------------------------------------------- receiving side
struct StateIncoming {
    Blob base;                     // last complete state received
    u32 base_ver = 0;
    u32 epoch = 0;                 // the sender's (a new one: everything held is dropped)
    // the versions held before the base, newest last: a slow line's sender builds the next version on one of
    // them before it has heard that a newer one arrived (one held only, every second version was refused)
    struct Older { u32 ver; Blob blob; };
    std::vector<Older> older;
    Blob partial;                  // delta being reassembled
    u32 part_ver = 0, part_have = 0, part_frags = 0;
    std::vector<unsigned char> got;
    bool fresh = false;            // base not yet loaded into the object
};
static std::unordered_map<u64, StateIncoming> g_incoming;

void statesync_on_packet(const unsigned char *data, int size, void (*send)(const void *, int), bool versus)
{
    g_translate = versus;
    const Head *head = (const Head *)data;
    if (head->type == PKT_OBJECT_ACK && size >= (int)sizeof(ObjectAck)) {
        acknowledged(g_own[0], (const ObjectAck *)data);
        return;
    }
    if (head->type != PKT_OBJECT || size < (int)sizeof(ObjectFrag)) return;
    const ObjectFrag *f = (const ObjectFrag *)data;
    u32 len = head->count;
    if (size < (int)(sizeof(ObjectFrag) + len) || f->frag >= f->nfrags || f->delta_size > (16u << 20)) return;
    StateIncoming &in = g_incoming[f->key];
    if (in.epoch != f->epoch) {             // the sender started again: nothing held is of its versions
        in.epoch = f->epoch;
        in.base.clear();
        in.base_ver = 0;
        in.older.clear();
        in.part_ver = 0;
    }
    ObjectAck ack = { make_head(PKT_OBJECT_ACK), f->key, f->version, f->epoch };
    if (f->version == in.base_ver && in.base_ver) { send(&ack, sizeof ack); return; }      // already have it: the ack was lost
    if (f->version < in.base_ver) {         // older than what is held (overtaken on the way): never gone back to
        if (f->frag == 0) { ack.version = in.base_ver; send(&ack, sizeof ack); }
        return;
    }
    const Blob *against = 0;
    static const Blob nothing;
    if (!f->base_version) against = &nothing;
    else if (f->base_version == in.base_ver) against = &in.base;
    else for (const StateIncoming::Older &o : in.older) if (o.ver == f->base_version) against = &o.blob;
    if (!against) {
        // built on a version we do not hold: say which one we do hold, so the owner can rebase
        if (f->frag == 0) { ack.version = in.base_ver; send(&ack, sizeof ack); g_stat_rejected++; }
        return;
    }
    if (in.part_ver != f->version || in.part_frags != f->nfrags) {
        in.part_ver = f->version;
        in.part_frags = f->nfrags;
        in.part_have = 0;
        in.got.assign(f->nfrags, 0);
        in.partial.assign(f->delta_size, 0);
    }
    if ((size_t)f->frag * k_frag + len > in.partial.size() || in.got[f->frag]) return;
    memcpy(in.partial.data() + (size_t)f->frag * k_frag, f + 1, len);
    in.got[f->frag] = 1;
    if (++in.part_have < in.part_frags) return;
    Blob next;
    if (!decode_delta(*against, in.partial.data(), in.partial.size(), f->raw_size, next)) { g_stat_bad++; return; }
    if (in.base_ver) {
        if (in.older.size() >= 2) in.older.erase(in.older.begin());
        in.older.push_back({ in.base_ver, Blob() });
        in.older.back().blob.swap(in.base);
    }
    in.base.swap(next);
    in.base_ver = f->version;
    in.part_ver = 0;
    in.fresh = true;
    g_stat_received += in.partial.size();
    send(&ack, sizeof ack);
}

// Before a simulation step: load every newly received state into its object.
void statesync_apply(ObjMan *om)
{
    static std::unordered_map<u64, Object *> index;
    static u32 seen_count;
    bool any = false;
    for (auto &kv : g_incoming) if (kv.second.fresh) { any = true; break; }
    if (!any) return;
    u32 count;
    Object **all = all_objects(om, &count);
    if (count != seen_count) {
        index.clear();
        for (u32 i = 0; i < count; i++) if (replicated(all[i])) index[statesync_key(all[i])] = all[i];
        seen_count = count;
    }
    for (auto &kv : g_incoming) {
        StateIncoming &in = kv.second;
        if (!in.fresh) continue;
        in.fresh = false;
        auto it = index.find(kv.first);
        if (it == index.end()) continue;
        Object *o = it->second;
        // debugging aids: MP_LOG_LOADS=1 names every state before it is loaded, MP_LOAD_ONLY=techn|human
        // restricts loading to one class
        static int trace = -1, only;
        if (trace < 0) {
            char v[16] = "";
            trace = GetEnvironmentVariableA("MP_LOG_LOADS", v, sizeof v) != 0;
            if (GetEnvironmentVariableA("MP_LOAD_ONLY", v, sizeof v)) only = v[0] == 't' ? 1 : 2;
        }
        if (only && strcmp(o->iface, only == 1 ? "i_techn" : "i_human")) continue;
        // (looked at for every unit, also one that is followed by position only: the comparison also tells
        // how the other game numbers this unit's handles - see state_local_handle)
        bool same_handles = !state_is_unit(o) || state_handles_match(o, in.base.data(), in.base.size());
        if (!session_state_wanted(o)) continue;
        if (!same_handles && !state_handles_translatable(o)) {
            g_stat_unsafe++;        // see state_handles_match: followed by position only
            continue;
        }
        if (trace) mp_log("load %s (%zu bytes)", o->full, in.base.size());
        // versus: the two games look at every unit from opposite sides (see state_load_keep_motion)
        int r = state_is_unit(o) ? state_load_keep_motion(o, in.base.data(), in.base.size(), g_translate)
                                 : state_load(o, in.base.data(), in.base.size());
        g_stat_loads++;
        if (r) g_stat_bad++;
    }
}

void statesync_report(void)
{
    state_translate_report();
    state_wheels_report();
    if (!g_stat_objects && !g_stat_loads && !g_stat_unsafe) return;
    mp_log("state: 5 s: sent %llu object states (%.1f KB/s on the wire for %.1f KB/s of state), loaded %llu (%.1f KB/s received, %llu rejected), %llu not loaded (render handles differ)",
           g_stat_objects, g_stat_bytes / 5120.0, g_stat_raw / 5120.0, g_stat_loads, g_stat_received / 5120.0, g_stat_bad, g_stat_unsafe);
    for (int ch = 0; ch < OWNERS; ch++) {
        Owner &o = g_own[ch];
        if ((ch && !o.on) || (!o.st_versions && !(ch == 0 && g_stat_rejected))) continue;
        char who[24] = "";
        if (ch) sprintf_s(who, "extra player %d: ", ch);
        mp_log("state: 5 s: %srate %.0f KB/s on average (now %.0f, %llu cuts; round trip %.0f ms, %.0f the least), %llu new versions sent, %llu acknowledged, %llu started over",
               who, o.rate_n ? o.rate_sum / o.rate_n : o.rate, o.rate, o.st_cuts, o.srtt, rtt_least(o), o.st_versions, o.st_acked, o.st_restarts);
        o.st_versions = o.st_acked = o.st_restarts = o.st_cuts = o.st_objects = o.st_bytes = 0;
        o.rate_sum = 0;
        o.rate_n = 0;
    }
    if (g_stat_rejected) mp_log("state: 5 s: %llu built on a version not held here", g_stat_rejected);
    g_stat_bytes = g_stat_objects = g_stat_raw = g_stat_loads = g_stat_bad = g_stat_received = g_stat_unsafe = 0;
    g_stat_rejected = 0;
    if (u64 bad = state_bad_lookups()) mp_log("state: 5 s: %llu model node look-ups ended early (a handle from the other game named no node here)", bad);
}
