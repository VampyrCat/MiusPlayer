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

int main(int argc, char **argv)
{
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
