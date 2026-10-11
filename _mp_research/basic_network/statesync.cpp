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
static void (*g_send)(const void *, int);
static u64 g_stat_bytes, g_stat_objects, g_stat_raw, g_stat_loads, g_stat_bad, g_stat_received, g_stat_unsafe;
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
static bool replicated_singleton(Object *o)
{
    static const char *names[] = { "i_divisions", "i_statistic", "i_trigman", "i_formation", "i_fireman", "i_shellphys" };
    static int enabled = -1;
    if (enabled < 0) {
        char v[8];
        enabled = GetEnvironmentVariableA("MP_SYNC_WORLD", v, sizeof v) ? atoi(v) : 0;
    }
    if (!enabled) return false;
    for (int i = 0; i < enabled && i < 6; i++)
        if (!strcmp(o->iface, names[i])) return true;
    return false;
}

static bool replicated(Object *o) { return state_is_unit(o) || replicated_singleton(o); }

// ---------------------------------------------------------------- owner side
struct Sent { u32 ver; Blob blob; };
struct Track {
    Blob base;                     // the newest version the other side has confirmed
    u32 base_ver = 0, last_ver = 0;
    std::vector<Sent> sent_versions;   // sent since then, newest last; any of them may be confirmed next
    DWORD checked = 0, sent = 0;
};
static std::unordered_map<u64, Track> g_tracks;
static u32 g_cursor;
static double g_budget;
static DWORD g_last_frame;

static void send_object(u64 key, Track &t, DWORD now)
{
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
        memcpy(h + 1, delta.data() + off, len);
        g_send(packet, (int)(sizeof(ObjectFrag) + len));
        g_budget -= sizeof(ObjectFrag) + len + 28;
        g_stat_bytes += sizeof(ObjectFrag) + len;
    }
    t.sent = now;
    g_stat_objects++;
    g_stat_raw += cur.size();
}

// Called every frame by the side that owns objects. `owns` decides per object.
void statesync_owner_frame(ObjMan *om, DWORD now, bool (*owns)(Object *), void (*send)(const void *, int))
{
    static int kbps = -1;
    static Blob blob;
    if (kbps < 0) {
        char v[16];
        kbps = GetEnvironmentVariableA("MP_STATE_KBPS", v, sizeof v) ? atoi(v) : 600;
    }
    g_send = send;
    double dt = g_last_frame ? (now - g_last_frame) / 1000.0 : 0;
    g_last_frame = now;
    g_budget += dt * kbps * 1024;
    if (g_budget > kbps * 1024 * 0.25) g_budget = kbps * 1024 * 0.25;       // at most a quarter second saved up
    u32 count;
    Object **all = all_objects(om, &count);
    for (u32 n = 0; n < count && g_budget > 0; n++) {
        Object *o = all[g_cursor++ % count];
        if (!replicated(o) || !owns(o) || !session_state_wanted(o)) continue;
        Track &t = g_tracks[statesync_key(o)];
        bool unit = state_is_unit(o);
        if (now - t.checked < (!strcmp(o->iface, "i_techn") ? 700u : 400u)) continue;
        t.checked = now;
        if (!state_save(o, blob)) continue;
        if (unit && blob.size() >= UNIT_MOTION_SIZE) memset(blob.data(), 0, UNIT_MOTION_SIZE);
        const Blob &newest = t.sent_versions.empty() ? t.base : t.sent_versions.back().blob;
        if (blob.size() == newest.size() && !memcmp(blob.data(), newest.data(), blob.size())) {
            if (!t.sent_versions.empty() && now - t.sent > 400) send_object(statesync_key(o), t, now);    // not confirmed: again
            continue;
        }
        if (t.sent_versions.size() >= 6) t.sent_versions.erase(t.sent_versions.begin());
        t.sent_versions.push_back({ ++t.last_ver, blob });
        send_object(statesync_key(o), t, now);
    }
}

void statesync_reset(void)
{
    g_tracks.clear();
}

// ---------------------------------------------------------------- receiving side
struct Incoming {
    Blob base;                     // last complete state received
    u32 base_ver = 0;
    Blob partial;                  // delta being reassembled
    u32 part_ver = 0, part_have = 0, part_frags = 0;
    std::vector<unsigned char> got;
    bool fresh = false;            // base not yet loaded into the object
};
static std::unordered_map<u64, Incoming> g_incoming;

void statesync_on_packet(const unsigned char *data, int size, void (*send)(const void *, int), bool versus)
{
    g_translate = versus;
    const Head *head = (const Head *)data;
    if (head->type == PKT_OBJECT_ACK && size >= (int)sizeof(ObjectAck)) {
        const ObjectAck *a = (const ObjectAck *)data;
        // The other side reports the version it now holds. Later sends are differences against that one.
        auto it = g_tracks.find(a->key);
        if (it == g_tracks.end() || a->version == it->second.base_ver) return;
        Track &t = it->second;
        for (size_t i = 0; i < t.sent_versions.size(); i++)
            if (t.sent_versions[i].ver == a->version) {
                t.base.swap(t.sent_versions[i].blob);
                t.base_ver = a->version;
                t.sent_versions.erase(t.sent_versions.begin(), t.sent_versions.begin() + i + 1);
                return;
            }
        // it holds something we no longer keep: start over from nothing
        t.base.clear();
        t.base_ver = 0;
        return;
    }
    if (head->type != PKT_OBJECT || size < (int)sizeof(ObjectFrag)) return;
    const ObjectFrag *f = (const ObjectFrag *)data;
    u32 len = head->count;
    if (size < (int)(sizeof(ObjectFrag) + len) || f->frag >= f->nfrags || f->delta_size > (16u << 20)) return;
    Incoming &in = g_incoming[f->key];
    ObjectAck ack = { make_head(PKT_OBJECT_ACK), f->key, f->version };
    if (f->version == in.base_ver && in.base_ver) { send(&ack, sizeof ack); return; }      // already have it: the ack was lost
    if (f->base_version != in.base_ver && f->base_version != 0) {
        // built on a version we do not hold: say which one we do hold, so the owner can rebase
        if (f->frag == 0) { ack.version = in.base_ver; send(&ack, sizeof ack); }
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
    static const Blob nothing;
    if (!decode_delta(f->base_version ? in.base : nothing, in.partial.data(), in.partial.size(), f->raw_size, next)) { g_stat_bad++; return; }
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
        Incoming &in = kv.second;
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
        if (!session_state_wanted(o)) continue;
        if (state_is_unit(o) && !state_handles_match(o, in.base.data(), in.base.size())) {
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
    if (!g_stat_objects && !g_stat_loads && !g_stat_unsafe) return;
    mp_log("state: 5 s: sent %llu object states (%.1f KB/s on the wire for %.1f KB/s of state), loaded %llu (%.1f KB/s received, %llu rejected), %llu not loaded (render handles differ)",
           g_stat_objects, g_stat_bytes / 5120.0, g_stat_raw / 5120.0, g_stat_loads, g_stat_received / 5120.0, g_stat_bad, g_stat_unsafe);
    g_stat_bytes = g_stat_objects = g_stat_raw = g_stat_loads = g_stat_bad = g_stat_received = g_stat_unsafe = 0;
    if (u64 bad = state_bad_lookups()) mp_log("state: 5 s: %llu model node look-ups ended early (a handle from the other game named no node here)", bad);
}
