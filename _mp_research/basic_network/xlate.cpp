// Numbers that mean different things in the two games.
//
// The engine's savegame code never writes a pointer. A pointer to a description (ammunition, weapon,
// equipment ...) is written as the number the object manager gave that description when it was registered,
// and a pointer to another object as that object's place in the object manager's list. In one game, and in
// a saved game loaded later, those numbers are simply right. Between two games running the same battle
// they are only right if both registered and created everything in the same order - true for a skirmish,
// not for an operation battle (seen in Saur-Mogila: 2697 descriptions in both games, 1432 of them under
// different numbers). A unit state from the other game loaded with the wrong numbers points at the wrong
// things: that was the crash of state replication in operation battles (adv_game+0xd9917: a "name" read
// through a description that was not one).
//
// So each game tells the other what its numbers stand for - per description a fingerprint of its contents,
// per object a hash of its name - and while a state of the other game is being loaded, the object manager's
// two look-ups (virtual slots 57: description by number, 54: object by place) are given the local number
// for the thing the other game meant. The tables travel on the shots' reliable channel (ShotRec.kind 3, 4).
#include "common.h"
#include "protocol.h"
#include <string.h>
#include <map>
#include <unordered_map>
#include <vector>

typedef void *(*Lookup)(ObjMan *, u32);
static Lookup g_obj_by_place, g_desc_by_num;
static std::unordered_map<u32, u32> g_desc, g_obj;          // the other game's number -> this game's
static std::vector<std::pair<u32, u32>> g_their_desc, g_their_obj;      // (number, fingerprint) as received
static u32 g_expect_desc, g_expect_obj, g_sent_desc, g_sent_obj;
static bool g_ready, g_hooked, g_send_due;
static void *g_vtbl_copy[97];

static u32 text_hash(const char *s, size_t max)
{
    u32 h = 2166136261u;
    for (size_t i = 0; i < max && s[i]; i++) h = (h ^ (unsigned char)s[i]) * 16777619u;
    return h;
}

// What a description is, whatever its address: its first 0x60 bytes, with every value that is a pointer
// replaced by the text it points at (names are) or left out.
static u32 fingerprint(const void *p)
{
    if (!readable(p, 0x60)) return 0;
    u64 w[12];
    memcpy(w, p, sizeof w);
    u32 h = 2166136261u;
    for (u64 v : w) {
        if (v >= 0x10000 && v < 0x800000000000ull && readable((void *)v, 16)) {
            const unsigned char *s = (const unsigned char *)v;
            int n = 0;
            while (n < 16 && s[n] >= 0x20 && s[n] < 0x7f) n++;
            v = n >= 3 && (n == 16 || !s[n]) ? text_hash((const char *)s, 64) : 0;
        }
        for (int i = 0; i < 8; i++) h = (h ^ (unsigned char)(v >> (i * 8))) * 16777619u;
    }
    return h ? h : 1;
}

static void local_tables(ObjMan *om, std::vector<std::pair<u32, u32>> &desc, std::vector<std::pair<u32, u32>> &obj)
{
    const char *base = (const char *)om;
    u32 n = *(const u32 *)(base + 0x1754);
    const char *rows = *(const char **)(base + 0x1748);
    if (n < 100000 && readable(rows, (size_t)n * 0x10))
        for (u32 i = 0; i < n; i++)
            desc.push_back({ *(const u32 *)(rows + (size_t)i * 0x10), fingerprint(*(void **)(rows + (size_t)i * 0x10 + 8)) });
    u32 m = *(const u32 *)(base + 0x15c);
    Object **list = *(Object ***)(base + 0x150);
    if (m < 1000000 && readable(list, (size_t)m * 8))
        for (u32 i = 0; i < m; i++)
            if (readable(list[i], sizeof(Object))) obj.push_back({ i, text_hash(list[i]->full, sizeof list[i]->full) });
}

static void send_table(const std::vector<std::pair<u32, u32>> &t, unsigned char kind)
{
    const size_t per = (sizeof(((ShotRec *)0)->launch) - 8) / 8;
    for (size_t at = 0; at < t.size() || at == 0; at += per) {
        ShotRec s = {};
        s.kind = kind;
        u32 total = (u32)t.size(), n = (u32)(t.size() - at < per ? t.size() - at : per);
        memcpy(s.launch, &total, 4);
        memcpy(s.launch + 4, &n, 4);
        for (u32 i = 0; i < n; i++) memcpy(s.launch + 8 + i * 8, &t[at + i], 8);
        shots_queue(s);
        if (t.empty()) break;
    }
}

// The other game's numbers matched to this game's. Both games hold the same things, registered in runs
// (everything one kind of unit needs, then the next kind's ...) - the runs come in another order, inside a
// run the order is the same. So: what has a fingerprint found exactly once in each game is matched by it
// (an anchor); everything else is matched by its distance from the nearest anchor before it, or failing
// that the nearest one after it. (A fingerprint alone is not enough for the rest: about half the
// descriptions hold values in their first bytes that differ from game to game.)
static void match(const std::vector<std::pair<u32, u32>> &theirs, const std::vector<std::pair<u32, u32>> &mine,
                  std::unordered_map<u32, u32> &out, const char *what)
{
    std::map<u32, int> count_t, count_m;
    std::map<u32, size_t> where_m;
    for (auto &t : theirs) count_t[t.second]++;
    for (size_t j = 0; j < mine.size(); j++) { count_m[mine[j].second]++; where_m[mine[j].second] = j; }
    const size_t none = (size_t)-1;
    std::vector<size_t> to(theirs.size(), none);            // place in `mine` for each of `theirs`
    std::vector<char> taken(mine.size(), 0);
    u32 anchors = 0, by_place = 0, by_place_same = 0, lost = 0, same = 0, moved = 0;
    for (size_t i = 0; i < theirs.size(); i++) {
        u32 fp = theirs[i].second;
        if (fp && count_t[fp] == 1 && count_m.count(fp) && count_m[fp] == 1) { to[i] = where_m[fp]; taken[to[i]] = 1; anchors++; }
    }
    for (int pass = 0; pass < 2; pass++) {
        // pass 0: from the anchor before; pass 1: from the anchor after
        size_t ai = none;
        for (size_t n = 0; n < theirs.size(); n++) {
            size_t i = pass ? theirs.size() - 1 - n : n;
            if (to[i] != none && (pass || true)) {
                // only true anchors and earlier results carry on a run
                ai = i;
                continue;
            }
            if (ai == none) continue;
            long long j = (long long)to[ai] + ((long long)i - (long long)ai);
            if (j < 0 || j >= (long long)mine.size() || taken[(size_t)j]) { continue; }
            to[i] = (size_t)j;
            taken[(size_t)j] = 1;
            by_place++;
            if (mine[(size_t)j].second == theirs[i].second) by_place_same++;
            ai = i;
        }
    }
    out.clear();
    for (size_t i = 0; i < theirs.size(); i++) {
        if (to[i] == none) { lost++; continue; }
        out[theirs[i].first] = mine[to[i]].first;
        if (mine[to[i]].first == theirs[i].first) same++; else moved++;
    }
    mp_log("numbers: %zu %s of the other game: %u have the same number here, %u another one, %u could not be placed (%u matched by what they are, %u by their place in a run - %u of those look alike too)",
           theirs.size(), what, same, moved, lost, anchors, by_place, by_place_same);
}

void xlate_received(const ShotRec &s, ObjMan *om)
{
    u32 total, n;
    memcpy(&total, s.launch, 4);
    memcpy(&n, s.launch + 4, 4);
    std::vector<std::pair<u32, u32>> &t = s.kind == 3 ? g_their_desc : g_their_obj;
    u32 &expect = s.kind == 3 ? g_expect_desc : g_expect_obj;
    if (t.size() >= expect) { t.clear(); g_ready = false; }         // a new table begins
    expect = total;
    for (u32 i = 0; i < n && i < 17; i++) {
        std::pair<u32, u32> e;
        memcpy(&e, s.launch + 8 + i * 8, 8);
        t.push_back(e);
    }
    if (g_their_desc.size() >= g_expect_desc && g_their_obj.size() >= g_expect_obj && g_expect_obj) {
        std::vector<std::pair<u32, u32>> desc, obj;
        local_tables(om, desc, obj);
        match(g_their_desc, desc, g_desc, "descriptions");
        match(g_their_obj, obj, g_obj, "objects");
        g_ready = true;
    }
}

bool xlate_ready(void) { return g_ready; }
u32 xlate_desc(u32 theirs) { auto it = g_desc.find(theirs); return it == g_desc.end() ? theirs : it->second; }

static void *obj_by_place_hook(ObjMan *om, u32 place)
{
    if (g_ready && state_remote_loading()) { auto it = g_obj.find(place); if (it != g_obj.end()) place = it->second; }
    return g_obj_by_place(om, place);
}

static void *desc_by_num_hook(ObjMan *om, u32 num)
{
    if (g_ready && state_remote_loading()) num = xlate_desc(num);
    return g_desc_by_num(om, num);
}

void xlate_resend(void) { g_send_due = true; g_ready = false; g_their_desc.clear(); g_their_obj.clear(); g_expect_desc = g_expect_obj = 0; }

// Every frame of a connected battle: this game's tables go out once the battle stands, and again when
// something new has been registered.
void xlate_frame(ObjMan *om)
{
    if (!session_connected() || session_role() == ROLE_NONE) return;
    if (!g_hooked) {
        g_hooked = true;
        HMODULE shell = GetModuleHandleA("base_shell.codelib");
        void **vt = om->vtbl;
        if (shell && (char *)vt == (char *)shell + 0x4ca10 && (char *)vt[54] == (char *)shell + 0x98e0 && (char *)vt[57] == (char *)shell + 0x121d0) {
            memcpy(g_vtbl_copy, vt - 1, sizeof g_vtbl_copy);
            g_obj_by_place = (Lookup)vt[54];
            g_desc_by_num = (Lookup)vt[57];
            g_vtbl_copy[1 + 54] = obj_by_place_hook;
            g_vtbl_copy[1 + 57] = desc_by_num_hook;
            om->vtbl = g_vtbl_copy + 1;
            mp_log("numbers: the other game's description and object numbers are translated when its states are loaded");
        } else {
            mp_log("numbers: unknown game build, the other game's numbers are taken as they are");
        }
        g_send_due = true;
    }
    static DWORD last;
    DWORD now = GetTickCount();
    if (now - last < 3000) return;
    last = now;
    u32 nd = *(u32 *)((char *)om + 0x1754), no = *(u32 *)((char *)om + 0x15c);
    if (!g_send_due && nd == g_sent_desc && no == g_sent_obj) return;
    std::vector<std::pair<u32, u32>> desc, obj;
    local_tables(om, desc, obj);
    send_table(desc, 3);
    send_table(obj, 4);
    g_sent_desc = nd;
    g_sent_obj = no;
    g_send_due = false;
}
