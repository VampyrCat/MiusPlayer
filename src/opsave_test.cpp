// Offline checks of the operation-save handling used for the shared operational map.
//   opsave_test <mine.opsave> <base.opsave> <theirs.opsave> <out.opsave>
//       the merge: mine = the host's save, base = the copy the other player was given, theirs = what he sent back
//   opsave_test mirror <save.opsave> [out.opsave]
//       what the other player would be given: who leads what before and after, by side
#include <stdio.h>
#include <string.h>
#include <map>
#include <string>
#include <vector>
#include "opsave.h"

static bool load(const char *path, std::vector<unsigned char> &out)
{
    FILE *f = 0;
    if (fopen_s(&f, path, "rb") || !f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? n : 0);
    size_t got = n > 0 ? fread(out.data(), 1, n, f) : 0;
    fclose(f);
    return got == out.size() && n > 0;
}

static std::string code(unsigned v)
{
    char s[5] = { 0 };
    memcpy(s, &v, 4);
    return v ? s : "-";
}

static void summary(const char *label, const std::vector<unsigned char> &b)
{
    std::map<std::string, std::map<std::string, int>> by;      // side -> tag -> count
    std::map<std::string, std::map<unsigned, int>> vis;
    for (const opsave::Tagged &t : opsave::tagged(b)) {
        std::string side = t.side ? code(opsave::u32_at(b, t.side)) : "?";
        by[side][code(opsave::u32_at(b, t.ai))]++;
        if (t.vis && t.px) vis[side][opsave::u32_at(b, t.vis)]++;
    }
    printf("%s:", label);
    for (auto &s : by) {
        printf("  %s led by", s.first.c_str());
        for (auto &t : s.second) printf(" %s x%d", t.first.c_str(), t.second);
        printf(" (battle groups seen:");
        for (auto &v : vis[s.first]) printf(" mode %u x%d", v.first, v.second);
        printf(")");
    }
    printf("\n");
}

// The other player's knowledge across turns: three of the host's battle groups fight (turn flag 0x10) in
// one copy; in the next copy, made without that flag, they must still be identified for him.
static int known_test(const std::vector<unsigned char> &original)
{
    std::vector<unsigned char> first = original, second = original;
    std::vector<opsave::Tagged> all = opsave::tagged(first);
    unsigned host = opsave::user_side(first, all), marked = 0;
    std::vector<std::string> fought;
    for (const opsave::Tagged &t : all) {
        if (marked >= 3 || t.pool || !t.flags || !opsave::on_side(first, t, host)) continue;
        unsigned v = opsave::u32_at(first, t.flags) | opsave::k_fought;
        memcpy(first.data() + t.flags, &v, 4);
        fought.push_back(t.name);
        marked++;
    }
    opsave::Known known;
    opsave::Sidecar side;
    unsigned changed = 0, id1 = 0, id2 = 0;
    if (!opsave::mirror(first, &side, &changed, &known, &id1)) { printf("cannot be turned round\n"); return 1; }
    if (!opsave::write_known("known_test.txt", opsave::operation_name(original), known)) return 1;
    opsave::Known again;
    opsave::read_known("known_test.txt", opsave::operation_name(original), &again);
    remove("known_test.txt");
    if (!opsave::mirror(second, &side, &changed, &again, &id2)) return 1;
    unsigned ok = 0;
    for (const opsave::Tagged &t : opsave::tagged(second))
        for (const std::string &n : fought)
            if (t.name == n && t.vis && (opsave::u32_at(second, t.vis) & 2)) ok++;
    printf("turn 1: %u groups fought, %u identified; turn 2 (no fighting): %u identified, %u of the %u still shown as identified: %s\n",
           marked, id1, id2, ok, marked, ok == marked && id2 == marked ? "OK" : "WRONG");
    return ok == marked && id2 == marked ? 0 : 1;
}

// What the other player's intelligence found out of the host's groups: two marked with turn flag 0x2000,
// two others named by uid (as the session notes them while the host's game carries a turn out); all four
// must be identified for him, and nothing of his own side.
static int intel_test(const std::vector<unsigned char> &original)
{
    std::vector<unsigned char> b = original;
    std::vector<opsave::Tagged> all = opsave::tagged(b);
    unsigned host = opsave::user_side(b, all), flagged = 0;
    std::set<unsigned> uids;
    std::vector<std::string> want;
    for (const opsave::Tagged &t : all) {
        if (t.pool || !t.flags || !t.uid || !opsave::on_side(b, t, host)) continue;
        if (flagged < 2) {
            unsigned v = opsave::u32_at(b, t.flags) | opsave::k_intel;
            memcpy(b.data() + t.flags, &v, 4);
            flagged++;
        } else if (uids.size() < 2) uids.insert(opsave::u32_at(b, t.uid));
        else break;
        want.push_back(t.name);
    }
    for (const opsave::Tagged &t : all)                      // a group of his own side found out: not his to learn
        if (!t.pool && t.flags && !opsave::on_side(b, t, host)) { unsigned v = opsave::u32_at(b, t.flags) | opsave::k_intel; memcpy(b.data() + t.flags, &v, 4); break; }
    opsave::Known known, before;
    size_t already = opsave::learn_fought(original, &before);          // (groups the save marks as having fought)
    unsigned a = opsave::learn_fought(b, &known), c = opsave::learn_uids(b, uids, &known), ok = 0;
    for (const std::string &n : want) ok += known.count(n) && (known[n] & 2);
    bool right = ok == want.size() && known.size() == want.size() + already;
    printf("intelligence: %u flagged, %u by uid (%zu fought before); learned %u + %u, %u of the %zu identified, %zu known in all: %s\n",
           flagged, (unsigned)uids.size(), already, a, c, ok, want.size(), known.size(), right ? "OK" : "WRONG");
    return right ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "intel")) {
        std::vector<unsigned char> save;
        if (!load(argv[2], save)) { printf("cannot read the save\n"); return 1; }
        return intel_test(save);
    }
    if (argc >= 3 && !strcmp(argv[1], "fought")) {          // which of the host's groups a battle start makes identified
        std::vector<unsigned char> save;
        if (!load(argv[2], save)) { printf("cannot read the save\n"); return 1; }
        opsave::Known known;
        unsigned n = opsave::learn_fought(save, &known);
        printf("%u of the host's battle groups fight in this save's battle:", n);
        for (auto &kv : known) printf(" %s", kv.first.c_str());
        printf("\n");
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "known")) {
        std::vector<unsigned char> save;
        if (!load(argv[2], save)) { printf("cannot read the save\n"); return 1; }
        return known_test(save);
    }
    if (argc >= 3 && !strcmp(argv[1], "mirror")) {
        std::vector<unsigned char> save;
        if (!load(argv[2], save)) { printf("cannot read the save\n"); return 1; }
        std::string name = opsave::operation_name(save), other = opsave::counterpart(name);
        printf("operation %s, other side's operation %s, %u turns, turn phase %d\n", name.c_str(), other.empty() ? "(none)" : other.c_str(),
               opsave::turns(save), opsave::turn_phase(save));
        summary("host's save   ", save);
        opsave::Sidecar side;
        unsigned changed = 0;
        if (!opsave::mirror(save, &side, &changed)) { printf("this save cannot be turned round\n"); return 1; }
        summary("other player's", save);
        std::vector<unsigned char> turn = save;
        opsave::unmirror(turn, side, 0);
        unsigned led = opsave::all_player_led(turn);
        summary("during a turn ", turn);
        unsigned back = opsave::restore_tags(turn, side);
        summary("after the turn", turn);
        printf("%u structs turned round, %u player-led for the turn, %u put back\n", changed, led, back);
        if (argc >= 4) {
            FILE *f = 0;
            if (fopen_s(&f, argv[3], "wb") || !f) return 1;
            fwrite(save.data(), 1, save.size(), f);
            fclose(f);
        }
        return 0;
    }
    if (argc < 5) { printf("usage: opsave_test <mine> <base> <theirs> <out>  |  opsave_test mirror <save> [out]\n"); return 2; }
    std::vector<unsigned char> mine, base, theirs;
    if (!load(argv[1], mine) || !load(argv[2], base) || !load(argv[3], theirs)) { printf("cannot read the saves\n"); return 1; }
    printf("orders before: mine %u, base %u, theirs %u\n", opsave::rows_of(mine, "user_orders"), opsave::rows_of(base, "user_orders"), opsave::rows_of(theirs, "user_orders"));
    unsigned orders = 0, cells = opsave::merge_moves(mine, base, theirs, &orders);
    printf("merged: %u orders appended, %u values changed; orders now %u; turn phase %d; %zu bytes\n", orders, cells,
           opsave::rows_of(mine, "user_orders"), opsave::turn_phase(mine), mine.size());
    FILE *f = 0;
    if (fopen_s(&f, argv[4], "wb") || !f) return 1;
    fwrite(mine.data(), 1, mine.size(), f);
    fclose(f);
    return 0;
}
